/* ==========================================================================
 * test_tracker.c — unit tests for the pattern-sequencer playback engine.
 *
 * Checks: row/tick timing math (samples_per_row from speed & tempo), that a
 * held note is released before a replacing note, NOTE_OFF releases, events land
 * on the correct absolute sample, the order list is traversed and a non-looping
 * song finishes at total_samples, looping never finishes, event-buffer draining
 * across small advance windows preserves order, and bad args are rejected.
 * ========================================================================== */
#include "cathode/tracker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0, checks = 0;
static void check(const char *n, int ok) {
    checks++;
    if (ok) printf("  ok   %s\n", n);
    else { printf("  FAIL %s\n", n); failures++; }
}

/* helper: make a cell */
static TrackerCell C(u8 note, u8 instr, u8 vol) {
    TrackerCell c; c.note = note; c.instrument = instr; c.volume = vol; c.effect = 0;
    return c;
}
/* helper: make a cell with an effect byte */
static TrackerCell CE(u8 note, u8 instr, u8 vol, u8 fx) {
    TrackerCell c; c.note = note; c.instrument = instr; c.volume = vol; c.effect = fx;
    return c;
}

int main(void) {
    printf("== CATHODE tracker tests ==\n");

    const u32 SR = 44100, SPEED = 6, TEMPO = 125;
    /* expected: tick = 44100*2.5/125 = 882 samples; row = 6*882 = 5292 */
    const u64 EXP_ROW = 5292;

    /* --- 1 channel, 1 pattern, 4 rows: C-E-off-G --- */
    {
        u32 rows = 4, ch = 1;
        TrackerCell cells[4];
        cells[0] = C(60, 1, 64);   /* C4 on */
        cells[1] = C(64, 1, 64);   /* E4 (replaces C) */
        cells[2] = C(NOTE_OFF, 0, 0);
        cells[3] = C(67, 2, 32);   /* G4, instr 2, half volume */
        u8 order[1] = {0};
        Tracker *t = tracker_create(cells, 1, rows, ch, order, 1, SR, SPEED, TEMPO, 0);
        check("create returns tracker", t != NULL);
        check("total_samples = rows*row_samples",
              tracker_total_samples(t) == (u64)rows * EXP_ROW);
        check("channels reported", tracker_channels(t) == 1);

        /* pull all events in one big window */
        TrackerEvent ev[32];
        u32 n = tracker_advance(t, (u32)(rows * EXP_ROW), ev, 32);
        /* expected event sequence:
         *   row0: ON C4  @0
         *   row1: OFF C4 @row, ON E4 @row
         *   row2: OFF E4 @2row
         *   row3: ON G4 @3row
         * = 5 events */
        check("row0 first event is NOTE_ON C4 at sample 0",
              n >= 1 && ev[0].kind == TEV_NOTE_ON && ev[0].note == 60 && ev[0].sample == 0);
        check("total event count is 5", n == 5);
        /* find the two row1 events */
        int off_before_on = 0;
        for (u32 i = 0; i + 1 < n; ++i)
            if (ev[i].sample == EXP_ROW && ev[i].kind == TEV_NOTE_OFF && ev[i].note == 60 &&
                ev[i+1].sample == EXP_ROW && ev[i+1].kind == TEV_NOTE_ON && ev[i+1].note == 64)
                off_before_on = 1;
        check("replacing note: OFF(C4) precedes ON(E4) at same sample", off_before_on);
        /* NOTE_OFF row */
        int found_off = 0;
        for (u32 i = 0; i < n; ++i)
            if (ev[i].sample == 2*EXP_ROW && ev[i].kind == TEV_NOTE_OFF && ev[i].note == 64) found_off = 1;
        check("NOTE_OFF releases held E4 at row2", found_off);
        /* G4 with instrument 2 and half velocity */
        int found_g = 0;
        for (u32 i = 0; i < n; ++i)
            if (ev[i].note == 67 && ev[i].kind == TEV_NOTE_ON) {
                found_g = (ev[i].instrument == 2 && ev[i].velocity > 0.45f && ev[i].velocity < 0.55f
                           && ev[i].sample == 3*EXP_ROW);
            }
        check("G4: instrument 2, ~0.5 velocity, at row3", found_g);
        check("finished after consuming whole song", tracker_finished(t));
        tracker_destroy(t);
    }

    /* --- streamed draining: same song, but advance in tiny windows --- */
    {
        u32 rows = 4, ch = 1;
        TrackerCell cells[4] = { C(60,1,64), C(64,1,64), C(NOTE_OFF,0,0), C(67,1,64) };
        u8 order[1] = {0};
        Tracker *t = tracker_create(cells, 1, rows, ch, order, 1, SR, SPEED, TEMPO, 0);
        u64 last_sample = 0; int monotonic = 1; u32 total = 0;
        u64 played = 0, dur = tracker_total_samples(t);
        while (played < dur) {
            TrackerEvent ev[8];
            u32 n = tracker_advance(t, 777, ev, 8);   /* odd window size */
            for (u32 i = 0; i < n; ++i) {
                if (ev[i].sample < last_sample) monotonic = 0;
                last_sample = ev[i].sample;
                total++;
            }
            played += 777;
        }
        check("streamed: events remain time-ordered across windows", monotonic);
        check("streamed: same 5 events as one-shot", total == 5);
        tracker_destroy(t);
    }

    /* --- order list of 2 patterns, plus looping never finishes --- */
    {
        u32 rows = 2, ch = 1;
        /* pattern 0: one note; pattern 1: one note */
        TrackerCell cells[4] = { C(60,1,64), C(NOTE_NONE,0,0),   /* pat0 */
                                 C(72,1,64), C(NOTE_NONE,0,0) }; /* pat1 */
        u8 order[3] = {0,1,0};
        Tracker *t = tracker_create(cells, 2, rows, ch, order, 3, SR, SPEED, TEMPO, 0);
        check("order: total = orderlen*rows*row_samples",
              tracker_total_samples(t) == 3ull * rows * EXP_ROW);
        TrackerEvent ev[32];
        u32 n = tracker_advance(t, (u32)(3*rows*EXP_ROW), ev, 32);
        /* Each pattern's row0 has a note; a held note is released first, so the
         * ON events are 60@0, 72@2row, 60@4row (with OFFs interleaved). Filter
         * to just the NOTE_ON events and check their note/sample sequence. */
        u32 nons = 0; TrackerEvent ons[8];
        for (u32 i = 0; i < n; ++i) if (ev[i].kind == TEV_NOTE_ON) ons[nons++] = ev[i];
        int ok = (nons == 3) &&
                 ons[0].note==60 && ons[0].sample==0 &&
                 ons[1].note==72 && ons[1].sample==2*EXP_ROW &&
                 ons[2].note==60 && ons[2].sample==4*EXP_ROW;
        check("order list plays patterns in sequence", ok);
        tracker_destroy(t);

        /* looping tracker reports total 0 and never finishes */
        Tracker *tl = tracker_create(cells, 2, rows, ch, order, 3, SR, SPEED, TEMPO, 1);
        check("looping: total_samples == 0", tracker_total_samples(tl) == 0);
        TrackerEvent e2[64];
        (void)tracker_advance(tl, (u32)(10*rows*EXP_ROW), e2, 64);
        check("looping: never finished", !tracker_finished(tl));
        tracker_destroy(tl);
    }

    /* ---- effects: tick-level granularity ---- */
    const u64 EXP_TICK = 882;   /* one tick = 44100*2.5/125 */

    /* note delay (0xDy): the note-on lands y ticks into the row, not at row 0 */
    {
        u32 rows = 1, ch = 1;
        TrackerCell cells[1] = { CE(60, 1, 64, TRK_FX(FX_NOTE_DELAY, 3)) };
        u8 order[1] = {0};
        Tracker *t = tracker_create(cells, 1, rows, ch, order, 1, SR, SPEED, TEMPO, 0);
        TrackerEvent ev[16];
        u32 n = tracker_advance(t, (u32)EXP_ROW, ev, 16);
        int ok = (n == 1) && ev[0].kind == TEV_NOTE_ON && ev[0].note == 60
                 && ev[0].sample == 3 * EXP_TICK;
        check("note delay 0xD3: ON lands at tick 3", ok);
        tracker_destroy(t);
    }

    /* note cut (0xCy): the note releases y ticks into the row */
    {
        u32 rows = 1, ch = 1;
        TrackerCell cells[1] = { CE(60, 1, 64, TRK_FX(FX_NOTE_CUT, 2)) };
        u8 order[1] = {0};
        Tracker *t = tracker_create(cells, 1, rows, ch, order, 1, SR, SPEED, TEMPO, 0);
        TrackerEvent ev[16];
        u32 n = tracker_advance(t, (u32)EXP_ROW, ev, 16);
        /* expect ON @0, OFF @ tick2 */
        int ok = (n == 2) && ev[0].kind == TEV_NOTE_ON && ev[0].sample == 0
                 && ev[1].kind == TEV_NOTE_OFF && ev[1].sample == 2 * EXP_TICK
                 && ev[1].note == 60;
        check("note cut 0xC2: OFF lands at tick 2", ok);
        tracker_destroy(t);
    }

    /* arpeggio (0x0y): pitch cycles base, base+y, base+2y over ticks 0,1,2 */
    {
        u32 rows = 1, ch = 1;
        TrackerCell cells[1] = { CE(60, 1, 64, TRK_FX(FX_ARPEGGIO, 4)) }; /* +4, +8 */
        u8 order[1] = {0};
        Tracker *t = tracker_create(cells, 1, rows, ch, order, 1, SR, SPEED, TEMPO, 0);
        TrackerEvent ev[32];
        u32 n = tracker_advance(t, (u32)EXP_ROW, ev, 32);
        /* collect NOTE_ON pitches in order */
        u8 pitches[16]; u32 np = 0;
        for (u32 i = 0; i < n; ++i) if (ev[i].kind == TEV_NOTE_ON) pitches[np++] = ev[i].note;
        /* speed=6 ticks: tick0=60, t1=64, t2=68, t3=60, t4=64, t5=68 */
        int ok = np >= 4 && pitches[0]==60 && pitches[1]==64 && pitches[2]==68 && pitches[3]==60;
        check("arpeggio 0x04: cycles 60,64,68,60 across ticks", ok);
        /* the arp base note should be the row's note (60) */
        check("arpeggio first ON is the base note", np>0 && pitches[0]==60);
        tracker_destroy(t);
    }

    /* --- bad args rejected --- */
    {
        TrackerCell c1 = C(60,1,64);
        u8 ord[1] = {0};
        check("null cells -> NULL", tracker_create(NULL,1,1,1,ord,1,SR,6,125,0)==NULL);
        check("zero rows -> NULL", tracker_create(&c1,1,0,1,ord,1,SR,6,125,0)==NULL);
        u8 bad[1] = {5};   /* references non-existent pattern */
        check("out-of-range order entry -> NULL",
              tracker_create(&c1,1,1,1,bad,1,SR,6,125,0)==NULL);
        check("advance(NULL) -> 0", tracker_advance(NULL,100,(TrackerEvent*)&c1,1)==0);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    if (!failures) printf("ALL PASS\n");
    return failures ? 1 : 0;
}
