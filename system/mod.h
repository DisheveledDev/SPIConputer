/* mod.h
 *
 * ProTracker MOD player, streamed from the SD card (Phase 8b).
 *
 * A module is 31 sample headers, an order list, up to 128 patterns of
 * 64 rows x 4 channels, and the samples' PCM (8-bit signed). Modules run
 * to hundreds of KB, almost all of it samples, so nothing but the
 * headers, the order list and two patterns is kept whole:
 *
 *  - Samples: a resident pool (MOD_POOL_BYTES) holds the short samples
 *    entirely (drums, most instruments) and the first MOD_HEAD_BYTES of
 *    the long ones, so every note starts at once. The rest of a long
 *    sample streams through a per-channel ring (MOD_RING_BYTES) that
 *    core 1 fills ahead of the read position from the file, between
 *    scheduler steps (mod_service), MOD_RING_CHUNK bytes a read. The
 *    ring is addressed by a stream position that keeps counting across
 *    loop wraps, so a long loop streams on without a break; a loop whose
 *    streamed stretch fits the ring is kept there and not read again. A
 *    note that outruns its ring plays silence until the data arrives
 *    (mod->underruns counts them).
 *  - Patterns: the one playing and the next in the order list; the
 *    player asks core 1 for the next pattern as it enters one, and for
 *    the target of a jump when it happens.
 *
 * Roles: core 1 loads (mod_load), services (mod_service) and requests
 * playback; the audio producer (core 0's audio_mix) runs the tick clock,
 * the effects and the four channels' mixing (mod_mix). The two sides
 * share the ring/pattern indices as volatile words in the same
 * single-producer/single-consumer shape as the video queue.
 *
 * Supported: M.K., M!K!, 4CHN and FLT4 modules (4 channels, 31 samples);
 * the effects 0-9, A-F and E1/E2/E5/E6/E9/EA/EB/EC/ED/EE. 6/8-channel
 * modules and 15-sample ones are refused.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MOD_CHANNELS 4
#define MOD_SAMPLES 31
#define MOD_ORDERS 128
#define MOD_ROWS 64
#define MOD_PATTERN_BYTES (MOD_ROWS * MOD_CHANNELS * 4)
#define MOD_HEADER_BYTES 1084

/* Memory: the pool, four rings and two patterns come from the system
 * heap, about 82 KB per loaded module (see AGENTS.md for the budget). */
#define MOD_POOL_BYTES (48u * 1024u) /* resident samples and heads */
#define MOD_HEAD_BYTES 2048u          /* resident start of a streamed sample */
#define MOD_RING_BYTES 8192u          /* per-channel streaming window */
#define MOD_RING_CHUNK 4096u          /* bytes read per service call per channel (<= RPC staging) */

typedef struct {
    uint32_t length;      /* frames (bytes) */
    uint32_t loop_start;  /* frames */
    uint32_t loop_len;    /* frames; 0 = no loop (the format's 0/2 marker is stored as 0) */
    uint32_t file_offset; /* of the PCM in the module file */
    int8_t finetune;      /* -8..7 */
    uint8_t volume;       /* 0..64 */
    const int8_t *resident; /* the whole sample, or its head, or NULL */
    uint32_t resident_len;
} mod_sample_t;

typedef struct {
    /* ---- streaming ring: core 1 fills ahead of core 0's reads ----
     * Positions are stream positions: frames counted from the note's
     * start, running on past a loop end (mod.c stream_frame maps one
     * back to a sample frame). The ring holds stream position p at
     * p % MOD_RING_BYTES. */
    int8_t *ring;
    volatile uint32_t fill_end;    /* positions [fill_end - MOD_RING_BYTES, fill_end) are valid */
    volatile uint32_t fill_start;  /* first valid position (after a restart) */
    volatile uint32_t read_pos;    /* core 0: the position it is playing */
    volatile uint8_t ring_sample;  /* which sample the ring holds */
    volatile uint32_t restart_seq; /* core 0 bumps: refill from read_pos */
    uint32_t restart_done;         /* core 1: the seq it has honoured */

    /* ---- playback (core 0) ---- */
    uint8_t sample;       /* 1..31, 0 = none */
    uint8_t note_sample;  /* sample number last seen in a cell */
    uint32_t pos;         /* sample frame */
    uint32_t spos;        /* stream position: pos before the first loop wrap */
    uint32_t frac;        /* Q16 fraction of a frame */
    uint32_t step;        /* Q16 frames per output frame */
    uint16_t period;      /* current (finetuned) period */
    uint16_t target;      /* tone portamento target */
    uint8_t volume;       /* 0..64 */
    int8_t pan;           /* -64..63 */
    uint8_t active;
    int32_t peak;         /* |output| max in the current block */
    volatile uint16_t level; /* 0..32767: decaying peak, for visuals (core 1 reads) */
    /* effect memory */
    uint8_t effect, param;
    uint8_t porta_speed, vib_param, trem_param, offset_param;
    uint8_t vib_pos, trem_pos;
    int16_t vib_delta, trem_delta;
    uint8_t loop_row, loop_count;
    uint8_t retrig, cut_tick, delay_tick;
    uint8_t arp_note;     /* note index (0..35) for arpeggio, 0xff none */
    uint16_t delayed_period;
    uint8_t delayed_sample;
} mod_channel_t;

typedef struct mod_s {
    int32_t handle;                 /* open file (core 1) */
    char name[21];
    mod_sample_t samples[MOD_SAMPLES + 1]; /* 1-based like the format */
    uint8_t order_count, restart_pos;
    uint8_t order[MOD_ORDERS];
    uint8_t pattern_count;
    uint32_t pattern_base;          /* file offset of pattern 0 */
    int8_t *pool;
    uint32_t pool_used;

    /* ---- patterns: two buffers, the playing one and the prefetch ---- */
    uint8_t *pat[2];
    volatile uint8_t pat_num[2];    /* pattern number held, 0xff = none */
    volatile uint8_t pat_want;      /* core 0: pattern it needs next (0xff none) */
    volatile uint8_t pat_want_slot; /* buffer to load it into */
    volatile uint32_t pat_seq, pat_done;

    /* ---- playback requests (core 1 -> core 0) ---- */
    volatile uint8_t req_play, req_stop, req_loop;

    /* ---- player state (core 0) ---- */
    volatile uint8_t playing;
    uint8_t loop;
    uint8_t speed, bpm, tick;
    uint8_t row, order_pos;
    uint8_t cur_slot;               /* pat[] slot of the playing pattern */
    uint32_t frames_per_tick, frame_acc;
    uint8_t pattern_delay;
    int16_t jump_order, jump_row;   /* -1 = none, set by B/D */
    uint8_t waiting;                /* pattern not loaded yet: silent */
    mod_channel_t ch[MOD_CHANNELS];
    uint32_t underruns;
    volatile uint32_t rows_played;
} mod_t;

/* Core 1: load a module from the card (header, order list, resident
 * samples, the first two patterns). Returns NULL and an error message. */
mod_t *mod_load(const char *path, const char **err);
void mod_free(mod_t *m);

/* Core 1: fill rings and patterns the player asked for. Cheap when
 * nothing is pending; call it every scheduler step. */
void mod_service(mod_t *m);

/* Core 1: playback requests, picked up by the producer's next block. */
void mod_request_play(mod_t *m, int loop);
void mod_request_stop(mod_t *m);
bool mod_is_playing(const mod_t *m);

/* Producer: apply requests, run the tick clock for `frames` frames and
 * mix the channels into l/r accumulators (stereo, one call per output
 * frame after mod_advance). */
void mod_advance(mod_t *m, int frames);
void mod_mix_frame(mod_t *m, int32_t *l, int32_t *r);

/* Silence the channels (program pause); playback resumes where it was. */
void mod_pause(mod_t *m);

/* For a tracker view (core 1). mod_note_name writes the tracker name of
 * an Amiga period ("C-2", "A#3"; "..." for 0) plus a NUL. mod_format_row
 * writes row `row` of pattern `pattern` as four "C-2 05 C40" cells
 * (note, sample in hex, effect+param) separated by spaces, if the
 * pattern is one of the two in RAM; false otherwise. */
void mod_note_name(uint16_t period, char out[4]);
bool mod_format_row(const mod_t *m, int pattern, int row, char *out, size_t cap);
