/* ==========================================================================
 * tracker.c — pattern-sequencer playback engine (see tracker.h).
 *
 * The engine walks the order list row by row. On each row start it looks at
 * every channel's cell and emits the note on/off events implied by the note
 * column, tracking per-channel "currently sounding" state so a NOTE_OFF (or a
 * new note that replaces a held one) releases the previous note. Events carry
 * absolute sample positions so a caller can render sample-accurate audio while
 * pulling events block by block.
 *
 * All timing is integer-sample based to stay perfectly reproducible: a row is
 * `speed * samples_per_tick` samples, where samples_per_tick derives from the
 * classic 2.5/tempo-seconds-per-tick rule.
 * ========================================================================== */
#include "cathode/tracker.h"
#include <stdlib.h>
#include <string.h>

/* Per-channel effect state, valid for the duration of the current row. */
typedef struct {
    u8  cur_note;    /* currently-sounding MIDI note (0 = silent) */
    u8  cur_instr;   /* last waveform chosen on this channel */
    /* effect being applied this row (decoded from the cell) */
    u8  fx_cmd;      /* command nibble, or 0xFF if none */
    u8  fx_param;    /* parameter nibble */
    u8  arp_base;    /* base note for arpeggio (the row's note) */
    u8  arp_last;    /* last arpeggio note emitted (so we can retrigger cleanly) */
    int did_delay;   /* note-delay: has the delayed note fired yet this row? */
    u8  delay_note;  /* the note/instr/vel to fire when the delay elapses */
    u8  delay_instr;
    f32 delay_vel;
    int did_cut;     /* note-cut: has the cut fired yet this row? */
} ChanState;

struct Tracker {
    TrackerCell *cells;      /* npatterns*rows*channels, owned copy */
    u32 npatterns, rows, channels;
    u8 *order; u32 orderlen;
    u32 sample_rate, speed, tempo;
    int loop;

    u64 samples_per_tick;    /* one tick */
    u64 samples_per_row;     /* speed * samples_per_tick */
    u64 total_samples;       /* full non-looping duration (0 if loop) */

    /* playback cursor, in ticks */
    u64 cursor;              /* absolute sample position consumed so far */
    u64 next_tick_sample;    /* sample index at which the next tick fires */
    u32 order_idx;           /* index into order[] */
    u32 row;                 /* row within the current pattern */
    u32 tick;                /* tick within the current row (0..speed-1) */
    int finished;

    ChanState *chan;         /* per-channel state, length `channels` */
};

static u64 samples_per_tick(u32 sample_rate, u32 tempo) {
    /* tick = 2.5 / tempo seconds; round to nearest sample, min 1 */
    double s = (double)sample_rate * 2.5 / (double)tempo;
    u64 v = (u64)(s + 0.5);
    return v ? v : 1;
}

Tracker *tracker_create(const TrackerCell *cells, u32 npatterns, u32 rows,
                        u32 channels, const u8 *order, u32 orderlen,
                        u32 sample_rate, u32 speed, u32 tempo, int loop) {
    if (!cells || !order || npatterns == 0 || rows == 0 || channels == 0 ||
        orderlen == 0 || sample_rate == 0) return NULL;
    if (speed == 0) speed = 6;
    if (tempo == 0) tempo = 125;
    /* validate order entries reference existing patterns */
    for (u32 i = 0; i < orderlen; ++i)
        if (order[i] >= npatterns) return NULL;

    Tracker *t = (Tracker *)calloc(1, sizeof(Tracker));
    if (!t) return NULL;
    size_t ncells = (size_t)npatterns * rows * channels;
    t->cells = (TrackerCell *)malloc(ncells * sizeof(TrackerCell));
    t->order = (u8 *)malloc(orderlen);
    t->chan  = (ChanState *)calloc(channels, sizeof(ChanState));
    if (!t->cells || !t->order || !t->chan) {
        tracker_destroy(t); return NULL;
    }
    memcpy(t->cells, cells, ncells * sizeof(TrackerCell));
    memcpy(t->order, order, orderlen);
    t->npatterns = npatterns; t->rows = rows; t->channels = channels;
    t->orderlen = orderlen; t->sample_rate = sample_rate;
    t->speed = speed; t->tempo = tempo; t->loop = loop;

    t->samples_per_tick = samples_per_tick(sample_rate, tempo);
    t->samples_per_row = (u64)speed * t->samples_per_tick;
    t->total_samples = loop ? 0
        : (u64)orderlen * rows * t->samples_per_row;

    t->cursor = 0;
    t->next_tick_sample = 0;  /* first tick triggers immediately at sample 0 */
    t->order_idx = 0; t->row = 0; t->tick = 0; t->finished = 0;
    for (u32 c = 0; c < channels; ++c) t->chan[c].fx_cmd = 0xFF;
    return t;
}

void tracker_destroy(Tracker *t) {
    if (!t) return;
    free(t->cells); free(t->order); free(t->chan);
    free(t);
}

u64 tracker_total_samples(const Tracker *t) { return t ? t->total_samples : 0; }
u32 tracker_channels(const Tracker *t) { return t ? t->channels : 0; }
int tracker_finished(const Tracker *t) { return t ? t->finished : 1; }

/* Small helpers to push one event; return the new count (bounded by max). */
static u32 emit_on(TrackerEvent *o, u32 n, u32 max, u64 at, u32 c,
                   u8 note, u8 instr, f32 vel) {
    if (n < max) {
        o[n].sample = at; o[n].channel = (u8)c; o[n].kind = TEV_NOTE_ON;
        o[n].note = note; o[n].instrument = instr; o[n].velocity = vel; ++n;
    }
    return n;
}
static u32 emit_off(TrackerEvent *o, u32 n, u32 max, u64 at, u32 c,
                    u8 note, u8 instr) {
    if (n < max) {
        o[n].sample = at; o[n].channel = (u8)c; o[n].kind = TEV_NOTE_OFF;
        o[n].note = note; o[n].instrument = instr; o[n].velocity = 0.0f; ++n;
    }
    return n;
}

/* Process TICK 0 of the current row for one channel: read the cell, decode its
 * effect, and (unless the note is delayed) trigger it. */
static u32 row_tick0(Tracker *t, u64 at, u32 c, TrackerEvent *out, u32 max, u32 n) {
    ChanState *ch = &t->chan[c];
    u32 pat = t->order[t->order_idx];
    const TrackerCell *cell =
        &t->cells[((size_t)pat * t->rows + t->row) * t->channels + c];

    /* decode effect for the whole row */
    ch->fx_cmd = 0xFF; ch->fx_param = 0;
    ch->did_delay = 1; ch->did_cut = 1;   /* default: nothing pending */
    if (cell->effect) {
        ch->fx_cmd = (u8)(cell->effect >> 4);
        ch->fx_param = (u8)(cell->effect & 0xF);
    }

    u8 note = cell->note;
    u8 instr = cell->instrument ? cell->instrument : ch->cur_instr;
    f32 vel = cell->volume ? (f32)cell->volume / 64.0f : 0.8f;

    if (note == NOTE_NONE) {
        /* no new note; arpeggio (if any) works off the currently-held note */
        if (ch->fx_cmd == FX_ARPEGGIO && ch->fx_param) {
            ch->arp_base = ch->cur_note; ch->arp_last = ch->cur_note;
        }
        return n;
    }
    if (note == NOTE_OFF) {
        if (ch->cur_note) { n = emit_off(out, n, max, at, c, ch->cur_note, ch->cur_instr); ch->cur_note = 0; }
        return n;
    }

    /* A real note. Note-delay defers the trigger to a later tick. */
    if (ch->fx_cmd == FX_NOTE_DELAY && ch->fx_param) {
        ch->did_delay = 0;                 /* pending */
        ch->delay_note = note; ch->delay_instr = instr; ch->delay_vel = vel;
        return n;                          /* do NOT release/trigger yet */
    }

    /* immediate trigger: release the previous, then start the new */
    if (ch->cur_note) n = emit_off(out, n, max, at, c, ch->cur_note, ch->cur_instr);
    n = emit_on(out, n, max, at, c, note, instr, vel);
    ch->cur_note = note; ch->cur_instr = instr;
    ch->arp_base = note; ch->arp_last = note;

    /* note-cut arms for a later tick this row */
    if (ch->fx_cmd == FX_NOTE_CUT) ch->did_cut = 0;
    return n;
}

/* Process an intra-row tick (>0) for one channel: apply per-tick effects. */
static u32 row_tickN(Tracker *t, u64 at, u32 c, u32 tick, TrackerEvent *out, u32 max, u32 n) {
    ChanState *ch = &t->chan[c];

    /* note delay: fire the deferred note exactly at its tick */
    if (!ch->did_delay && tick >= ch->fx_param) {
        if (ch->cur_note) n = emit_off(out, n, max, at, c, ch->cur_note, ch->cur_instr);
        n = emit_on(out, n, max, at, c, ch->delay_note, ch->delay_instr, ch->delay_vel);
        ch->cur_note = ch->delay_note; ch->cur_instr = ch->delay_instr;
        ch->arp_base = ch->delay_note; ch->arp_last = ch->delay_note;
        ch->did_delay = 1;
        if (ch->fx_cmd == FX_NOTE_CUT) ch->did_cut = 0;
        return n;
    }

    /* note cut: release after fx_param ticks */
    if (!ch->did_cut && tick >= ch->fx_param) {
        if (ch->cur_note) n = emit_off(out, n, max, at, c, ch->cur_note, ch->cur_instr);
        ch->cur_note = 0; ch->did_cut = 1;
        return n;
    }

    /* arpeggio: cycle 0, P, 2P semitones on successive ticks by retriggering */
    if (ch->fx_cmd == FX_ARPEGGIO && ch->fx_param && ch->arp_base) {
        u32 step = tick % 3;                 /* 0,1,2,0,1,2,... */
        int semis = (step == 0) ? 0 : (step == 1 ? ch->fx_param : 2 * ch->fx_param);
        int target = (int)ch->arp_base + semis;
        if (target > 127) target = 127;
        if ((u8)target != ch->arp_last) {
            /* retrigger: off the previous arp note, on the new pitch */
            n = emit_off(out, n, max, at, c, ch->arp_last, ch->cur_instr);
            n = emit_on(out, n, max, at, c, (u8)target, ch->cur_instr, 0.8f);
            ch->arp_last = (u8)target;
            ch->cur_note = (u8)target;
        }
    }
    return n;
}

/* Advance (order_idx,row,tick) by one tick; set finished at the song's end. */
static void step_tick(Tracker *t) {
    t->tick++;
    if (t->tick >= t->speed) {
        t->tick = 0;
        t->row++;
        if (t->row >= t->rows) {
            t->row = 0;
            t->order_idx++;
            if (t->order_idx >= t->orderlen) {
                if (t->loop) t->order_idx = 0;
                else t->finished = 1;
            }
        }
    }
}

u32 tracker_advance(Tracker *t, u32 nsamples, TrackerEvent *out, u32 max) {
    if (!t || !out || max == 0 || nsamples == 0) return 0;
    u32 n = 0;
    u64 window_end = t->cursor + nsamples;

    /* Fire every tick whose start falls within [cursor, window_end). */
    while (!t->finished && t->next_tick_sample < window_end) {
        u64 at = t->next_tick_sample;
        if (t->tick == 0) {
            for (u32 c = 0; c < t->channels; ++c)
                n = row_tick0(t, at, c, out, max, n);
        } else {
            for (u32 c = 0; c < t->channels; ++c)
                n = row_tickN(t, at, c, t->tick, out, max, n);
        }
        step_tick(t);
        t->next_tick_sample += t->samples_per_tick;
        if (n + t->channels * 2 > max) break;   /* leave headroom; caller drains */
    }
    t->cursor = window_end;
    return n;
}
