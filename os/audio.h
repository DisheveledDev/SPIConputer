/* audio.h
 *
 * Audio subsystem (Phase 8): an 8-voice stereo synth engine plus a
 * looping multi-channel score player and WAV sample playback, driven
 * from Lua.
 *
 * Model (see AGENTS.md, "Audio Subsystem"):
 *  - Programs define "sounds" (tone instruments: waveform + ADSR) and
 *    "scores" (per-channel event timelines). Definitions compile into
 *    flat buffers in this per-program state (outside the Lua heap).
 *  - Lua only requests playback (audio_play_score/audio_stop_score/
 *    audio_trigger); the score cursor, voices and mixer are owned by
 *    the audio producer (core 0 on firmware). audio_mix() renders
 *    interleaved stereo s16 blocks at AUDIO_SAMPLE_RATE; on the
 *    product board those blocks feed the HDMI data-island queue.
 *  - Like video state, each program owns an audio_state_t and the
 *    active one is published as g_current_audio by a pointer swap.
 *
 * Interrupt/threading note: definitions and requests are written by
 * core 1, the producer state by core 0. Requests are a few volatile
 * flags plus a small SPSC trigger ring; a request is picked up on the
 * next audio_mix() block. Torn definition reads are possible in
 * principle (same single-buffered policy as video, see Q18) and are
 * bounded to one block.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define AUDIO_SAMPLE_RATE 44100

#define AUDIO_VOICES 16
#define AUDIO_CHANNELS 8 /* score voices, voice index == channel */
#define AUDIO_ONESHOT_VOICES (AUDIO_VOICES - AUDIO_CHANNELS)

#define AUDIO_INSTRUMENT_MAX 32
#define AUDIO_SCORE_MAX 8
#define AUDIO_SCORE_EVENTS_MAX 1024 /* shared per-program event pool */
#define AUDIO_SCORE_NAME_MAX 12
#define AUDIO_CHANNEL_MAX 8 /* events per score channel cap (event pool) */

/* WAV playback: sample ids share the sound namespace with instruments
 * (ids AUDIO_INSTRUMENT_MAX + n), so scores can reference samples too. */
#define AUDIO_SAMPLE_MAX 8
#define AUDIO_SAMPLE_POOL_MAX (64u * 1024u)

#define AUDIO_WAVE_SQUARE 0
#define AUDIO_WAVE_TRIANGLE 1
#define AUDIO_WAVE_SAW 2
#define AUDIO_WAVE_SINE 3
#define AUDIO_WAVE_NOISE 4

/* Envelope (and one-shot) defaults. */
#define AUDIO_DEFAULT_VOLUME 255
#define AUDIO_DEFAULT_DUTY 8
#define AUDIO_ONESHOT_TAIL_MS 250

typedef struct {
    uint8_t defined;
    uint8_t wave;       /* AUDIO_WAVE_* */
    uint8_t duty;       /* square/pulse duty, 1..15 (of 16) */
    uint8_t attack_ms;  /* 0..255 */
    uint8_t decay_ms;   /* 0..255 */
    uint8_t sustain;    /* level 0..255 */
    uint8_t release_ms; /* 0..255 */
    uint8_t volume;     /* default volume 0..255 */
} audio_instrument_t;

typedef struct {
    uint32_t time_ms; /* absolute from score start */
    uint32_t dur_ms;  /* release begins after this; 0 = one-shot envelope */
    uint8_t sound;    /* instrument id or AUDIO_INSTRUMENT_MAX + sample id */
    uint8_t note;     /* MIDI note number (C4 = 60) */
    uint8_t volume;   /* 0..255, scales the instrument volume */
    int8_t pan;       /* -64 (left) .. 63 (right) */
} audio_event_t;

typedef struct {
    uint16_t start; /* index into audio_state_t.events */
    uint16_t count;
} audio_channel_t;

typedef struct {
    uint8_t valid;
    char name[AUDIO_SCORE_NAME_MAX];
    uint8_t loop; /* default loop flag (MusicPlay can override) */
    uint32_t length_ms;
    audio_channel_t ch[AUDIO_CHANNEL_MAX];
} audio_score_t;

typedef struct {
    uint8_t defined;
    uint8_t channels; /* 1 or 2 */
    uint16_t rate;    /* source sample rate */
    uint32_t frames;  /* per-channel frames */
    uint32_t offset;  /* byte offset into the pool (int16 samples) */
} audio_pcm_t;

/* A pending one-shot request (SPSC ring, core 1 -> producer). */
typedef struct {
    uint8_t sound;
    uint8_t note;
    uint8_t volume;
    int8_t pan;
    uint16_t dur_ms;
} audio_trigger_t;

/* Envelope stages. */
enum {
    AUDIO_ENV_OFF = 0,
    AUDIO_ENV_ATTACK,
    AUDIO_ENV_DECAY,
    AUDIO_ENV_SUSTAIN,
    AUDIO_ENV_RELEASE,
};

typedef struct {
    uint8_t active;
    uint8_t source;     /* 0 = tone instrument, 1 = PCM sample */
    uint8_t sound;      /* instrument id / sample id */
    uint8_t note;
    uint8_t volume;     /* 0..255, after instrument scaling */
    uint8_t lgain, rgain;
    uint32_t phase;     /* tone phase, Q32 */
    uint32_t inc;       /* tone phase increment, Q32/sample */
    uint64_t pos;       /* sample position, Q32.32 (interleaved frames) */
    uint64_t step;      /* sample position increment, Q32.32/frame */
    uint16_t lfsr;      /* noise generator state */
    uint8_t env_state;
    int32_t env_level;   /* 0..65536, Q16 */
    int32_t env_rate;    /* delta per frame, Q16 */
    int32_t env_sustain; /* Q16 */
    uint32_t hold_frames; /* frames until release (score dur / one-shot tail) */
} audio_voice_t;

typedef struct audio_state_s {
    /* ---- definitions (written by the Lua module, core 1) ---- */
    audio_instrument_t instruments[AUDIO_INSTRUMENT_MAX];
    audio_score_t scores[AUDIO_SCORE_MAX];
    audio_event_t events[AUDIO_SCORE_EVENTS_MAX];
    uint16_t events_used;
    audio_pcm_t samples[AUDIO_SAMPLE_MAX];
    int16_t *sample_pool;      /* malloc'd lazily on first SoundLoad */
    uint32_t sample_pool_used; /* bytes */
    uint32_t version;          /* bumped on any definition change */

    /* ---- playback requests (core 1 -> producer) ---- */
    volatile uint8_t master; /* 0..255 */
    volatile uint8_t req_play;
    volatile uint8_t req_score;
    volatile uint8_t req_loop;
    volatile uint8_t req_stop;
    volatile uint8_t stop_flags[AUDIO_ONESHOT_VOICES];
    audio_trigger_t trig[AUDIO_ONESHOT_VOICES];
    atomic_uint trig_head; /* producer writes */
    atomic_uint trig_tail; /* consumer reads */

    /* ---- producer state (core 0 / audio_mix) ---- */
    volatile uint8_t score_active;
    uint8_t score_loop;
    uint8_t score_index;
    uint32_t score_frame;
    uint16_t ch_next[AUDIO_CHANNEL_MAX];
    audio_voice_t voices[AUDIO_VOICES];
} audio_state_t;

/* The active audio state: points at the top program's state. Written
 * only by program push/pop (core 1); read by the audio producer. */
extern audio_state_t *g_current_audio;

/* Init a fresh state (silent, default master volume). */
void audio_state_init(audio_state_t *a);

/* Release the sample pool (heap). */
void audio_state_free(audio_state_t *a);

/* MIDI note number for a note name ("C4", "A#3", "Bb2", "C-1").
 * Returns -1 when the name is malformed. */
int audio_note_parse(const char *name);

/* True when `sound` is a defined instrument or a loaded sample. */
bool audio_sound_defined(const audio_state_t *a, int sound);

/* ---- playback requests (core 1) ---- */

/* Request the given score slot. loop < 0 keeps the score's own flag. */
void audio_play_score(audio_state_t *a, int score, int loop);
void audio_stop_score(audio_state_t *a);
bool audio_score_playing(const audio_state_t *a);

/* Post a one-shot. Returns the voice slot (0..AUDIO_ONESHOT_VOICES-1)
 * or -1 when the ring is full. dur_ms = 0 selects the one-shot envelope
 * (release after attack+decay). */
int audio_trigger(audio_state_t *a, int sound, int note, int volume, int pan,
                  int dur_ms);

/* Release a one-shot voice (slot from audio_trigger) / all one-shots. */
void audio_stop_voice(audio_state_t *a, int slot);
void audio_stop_all_voices(audio_state_t *a);

/* Silence every voice without touching the score cursor (program pause). */
void audio_pause(audio_state_t *a);

/* ---- producer (core 0 / host tests) ---- */

/* Render `frames` interleaved stereo s16 frames at AUDIO_SAMPLE_RATE.
 * Also applies any pending playback requests and advances the score
 * cursor. Safe to call with any block size. */
void audio_mix(audio_state_t *a, int16_t *out, int frames);

/* Convenience for one-shot envelope length: attack+decay frames. */
uint32_t audio_oneshot_frames(const audio_instrument_t *ins);
