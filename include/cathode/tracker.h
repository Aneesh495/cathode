/* ==========================================================================
 * cathode/tracker.h — a pattern-based music sequencer (a "tracker").
 *
 * This is the data model + playback clock of a classic MOD/XM-style tracker,
 * written in pure C so it is testable with no audio backend. It does NOT make
 * sound itself; it converts a song (patterns of note cells, an order list, a
 * speed/tempo clock) into a stream of sample-accurate note on/off EVENTS. A
 * caller (see `capture --audio --song`) feeds those events to the C++ synth's
 * `cpp_synth_note_on/off` and renders the audio.
 *
 * Timing model (Amiga/FastTracker conventions):
 *   - `speed`  = ticks per row (default 6).
 *   - `tempo`  = BPM (default 125). One tick lasts 2.5/tempo seconds, i.e.
 *                samples_per_tick = round(sample_rate * 2.5 / tempo).
 *   - a row therefore lasts speed*samples_per_tick samples; note events land on
 *     row boundaries (this simple engine triggers on row starts only).
 *
 * A pattern cell's `note` uses these sentinels:
 *   NOTE_NONE (0)  — no change this row (let the channel keep playing)
 *   NOTE_OFF (255) — release whatever is playing on this channel
 *   otherwise      — a MIDI note number (1..127); 60 = middle C.
 * `instrument` selects a synth waveform (0..3); 0 in a fresh cell means "keep".
 *
 * The `effect` byte carries a per-cell effect packed as a command nibble in the
 * high 4 bits and a parameter nibble in the low 4 bits (0xCP → command C,
 * param P). Supported commands (others are ignored):
 *   0x0  arpeggio: within the row, cycle the pitch through offsets 0, P, 2P
 *        semitones on successive ticks — the classic chiptune "chord" played
 *        by one channel by rapidly retriggering (P=3 → diminished-ish, P=4 →
 *        augmented). P=0 is a no-op.
 *   0xC  note cut:  release the note P ticks into the row (ECx-style).
 *   0xD  note delay: delay this cell's note-on until P ticks into the row.
 * These are emitted at TICK granularity, so a caller pulling events block by
 * block still gets them at the right absolute sample. Use the helper macro
 * TRK_FX(cmd,param) to build the byte.
 * ========================================================================== */
#ifndef CATHODE_TRACKER_H
#define CATHODE_TRACKER_H

#include "cathode/types.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { NOTE_NONE = 0, NOTE_OFF = 255 };

/* Effect command nibbles (high nibble of TrackerCell.effect). */
enum { FX_ARPEGGIO = 0x0, FX_NOTE_CUT = 0xC, FX_NOTE_DELAY = 0xD };
/* Build an effect byte from a command nibble and a 0..15 parameter. A byte of
 * 0 means "no effect" (FX_ARPEGGIO with param 0 is also a no-op). */
#define TRK_FX(cmd, param) ((u8)((((cmd) & 0xF) << 4) | ((param) & 0xF)))

/* One cell of one channel on one row. POD, packed small. */
typedef struct {
    u8 note;        /* NOTE_NONE / NOTE_OFF / MIDI 1..127 */
    u8 instrument;  /* 1..N selects waveform; 0 = keep current */
    u8 volume;      /* 0..64 (0 = keep previous), classic tracker volume col */
    u8 effect;      /* reserved for future effects; 0 = none */
} TrackerCell;

/* Event kinds emitted during playback. */
typedef enum { TEV_NOTE_ON, TEV_NOTE_OFF } TrackerEventKind;

typedef struct {
    u64 sample;     /* absolute sample index at which the event fires */
    u8  channel;    /* which channel */
    u8  kind;       /* TrackerEventKind */
    u8  note;       /* MIDI note (for NOTE_ON) */
    u8  instrument; /* waveform id (for NOTE_ON) */
    f32 velocity;   /* 0..1 (from volume column) */
} TrackerEvent;

typedef struct Tracker Tracker;

/* Build a tracker for a song laid out as: `npatterns` patterns, each `rows`
 * rows by `channels` channels of cells (row-major: cells[(p*rows+r)*channels+c]).
 * `order` is a length-`orderlen` list of pattern indices to play in sequence.
 * The song plays through the order list once (no looping) unless `loop` is set.
 * Returns NULL on invalid args. Copies the cell data (caller keeps ownership). */
Tracker *tracker_create(const TrackerCell *cells, u32 npatterns, u32 rows,
                        u32 channels, const u8 *order, u32 orderlen,
                        u32 sample_rate, u32 speed, u32 tempo, int loop);
void     tracker_destroy(Tracker *t);

/* Total samples the (non-looping) song will play. 0 if looping. */
u64  tracker_total_samples(const Tracker *t);
u32  tracker_channels(const Tracker *t);

/* Advance playback by `nsamples` and append the events that fire in that window
 * to `out` (up to `max` events). Returns the number of events written. The
 * engine tracks its own sample cursor across calls, so stream it block by
 * block. Returns 0 forever once a non-looping song has ended. */
u32  tracker_advance(Tracker *t, u32 nsamples, TrackerEvent *out, u32 max);

/* True once a non-looping song has played its whole order list. */
int  tracker_finished(const Tracker *t);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_TRACKER_H */
