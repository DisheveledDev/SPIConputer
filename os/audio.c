/* audio.c — see audio.h */
#include "audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

audio_state_t *g_current_audio;

/* ------------------------------------------------------------------ */
/* Fixed-point tables (built once, using double math)                  */
/* ------------------------------------------------------------------ */

enum { AUDIO_BLOCK = 64 };

static bool s_tables_ready;
static uint32_t s_note_inc[128];   /* tone phase increment, Q32/sample */
static uint64_t s_pitch_q32[128];  /* 2^((n-60)/12), Q32 (sample pitch) */
static int16_t s_sine[256];

static void build_tables(void) {
    if (s_tables_ready) {
        return;
    }
    for (int n = 0; n < 128; n++) {
        double freq = 440.0 * pow(2.0, (n - 69) / 12.0);
        s_note_inc[n] =
            (uint32_t)llround(freq / AUDIO_SAMPLE_RATE * 4294967296.0);
        s_pitch_q32[n] = (uint64_t)llround(pow(2.0, (n - 60) / 12.0) * 4294967296.0);
    }
    for (int i = 0; i < 256; i++) {
        s_sine[i] = (int16_t)lround(sin(2.0 * 3.14159265358979323846 * i / 256.0) *
                                    32767.0);
    }
    s_tables_ready = true;
}

static uint64_t ms_to_frames(uint32_t ms) {
    return (uint64_t)ms * AUDIO_SAMPLE_RATE / 1000u;
}

/* ------------------------------------------------------------------ */
/* Note names                                                          */
/* ------------------------------------------------------------------ */

int audio_note_parse(const char *name) {
    if (!name || !name[0]) {
        return -1;
    }
    char c = name[0];
    if (c >= 'a' && c <= 'g') {
        c = (char)(c - 'a' + 'A');
    }
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7}; /* A B C D E F G */
    if (c < 'A' || c > 'G') {
        return -1;
    }
    int i = 1;
    int semitone = base[c - 'A'];
    if (name[i] == '#') {
        semitone++;
        i++;
    } else if (name[i] == 'b') {
        semitone--;
        i++;
    }
    if (!name[i]) {
        return -1;
    }
    int sign = 1;
    if (name[i] == '-') {
        sign = -1;
        i++;
    }
    if (name[i] < '0' || name[i] > '9') {
        return -1;
    }
    int octave = name[i] - '0';
    i++;
    if (name[i] >= '0' && name[i] <= '9') {
        octave = octave * 10 + (name[i] - '0');
        i++;
    }
    if (name[i] != '\0') {
        return -1;
    }
    octave *= sign;
    int midi = (octave + 1) * 12 + semitone;
    if (midi < 0 || midi > 127) {
        return -1;
    }
    return midi;
}

/* ------------------------------------------------------------------ */
/* State lifetime                                                      */
/* ------------------------------------------------------------------ */

void audio_state_init(audio_state_t *a) {
    memset(a, 0, sizeof(*a));
    a->master = AUDIO_DEFAULT_VOLUME;
    build_tables();
}

void audio_state_free(audio_state_t *a) {
    free(a->sample_pool);
    a->sample_pool = NULL;
}

bool audio_sound_defined(const audio_state_t *a, int sound) {
    if (sound < 0) {
        return false;
    }
    if (sound < AUDIO_INSTRUMENT_MAX) {
        return a->instruments[sound].defined != 0;
    }
    if (sound < AUDIO_INSTRUMENT_MAX + AUDIO_SAMPLE_MAX) {
        return a->samples[sound - AUDIO_INSTRUMENT_MAX].defined != 0;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Playback requests (core 1 side)                                     */
/* ------------------------------------------------------------------ */

void audio_play_score(audio_state_t *a, int score, int loop) {
    if (score < 0 || score >= AUDIO_SCORE_MAX) {
        return;
    }
    a->req_score = (uint8_t)score;
    a->req_loop = loop < 0 ? a->scores[score].loop : (uint8_t)(loop != 0);
    a->req_play = 1;
}

void audio_stop_score(audio_state_t *a) {
    a->req_stop = 1;
}

bool audio_score_playing(const audio_state_t *a) {
    if (a->req_stop) {
        return false;
    }
    return a->score_active != 0 || a->req_play != 0;
}

int audio_trigger(audio_state_t *a, int sound, int note, int volume, int pan,
                  int dur_ms) {
    if (!audio_sound_defined(a, sound)) {
        return -1;
    }
    unsigned head = atomic_load_explicit(&a->trig_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&a->trig_tail, memory_order_acquire);
    if ((head + 1) % AUDIO_ONESHOT_VOICES == tail) {
        return -1; /* ring full */
    }
    int slot = (int)head;
    audio_trigger_t *t = &a->trig[slot];
    t->sound = (uint8_t)sound;
    t->note = (uint8_t)(note < 0 ? 0 : (note > 127 ? 127 : note));
    t->volume = (uint8_t)(volume < 0 ? 0 : (volume > 255 ? 255 : volume));
    t->pan = (int8_t)(pan < -64 ? -64 : (pan > 63 ? 63 : pan));
    t->dur_ms = (uint16_t)(dur_ms < 0 ? 0 : (dur_ms > 65535 ? 65535 : dur_ms));
    atomic_store_explicit(&a->trig_head,
                          (head + 1) % AUDIO_ONESHOT_VOICES,
                          memory_order_release);
    return slot;
}

void audio_stop_voice(audio_state_t *a, int slot) {
    if (slot < 0 || slot >= AUDIO_ONESHOT_VOICES) {
        return;
    }
    a->stop_flags[slot] = 1;
}

void audio_stop_all_voices(audio_state_t *a) {
    for (int i = 0; i < AUDIO_ONESHOT_VOICES; i++) {
        a->stop_flags[i] = 1;
    }
}

void audio_pause(audio_state_t *a) {
    for (int i = 0; i < AUDIO_VOICES; i++) {
        a->voices[i].active = 0;
        a->voices[i].env_state = AUDIO_ENV_OFF;
    }
}

/* ------------------------------------------------------------------ */
/* Voice triggering                                                    */
/* ------------------------------------------------------------------ */

static const audio_instrument_t s_sample_envelope = {
    .defined = 1,
    .wave = AUDIO_WAVE_SINE,
    .duty = AUDIO_DEFAULT_DUTY,
    .attack_ms = 0,
    .decay_ms = 0,
    .sustain = 255,
    .release_ms = 20,
    .volume = 255,
};

static uint32_t oneshot_hold(const audio_instrument_t *ins, uint32_t dur_ms) {
    if (dur_ms > 0) {
        return (uint32_t)ms_to_frames(dur_ms);
    }
    return (uint32_t)(ms_to_frames(ins->attack_ms) + ms_to_frames(ins->decay_ms));
}

uint32_t audio_oneshot_frames(const audio_instrument_t *ins) {
    return (uint32_t)(ms_to_frames(ins->attack_ms) + ms_to_frames(ins->decay_ms));
}

static void env_enter_decay(audio_voice_t *v, const audio_instrument_t *ins) {
    v->env_sustain = (int32_t)ins->sustain * 256;
    uint32_t df = (uint32_t)ms_to_frames(ins->decay_ms);
    if (df == 0 || v->env_level <= v->env_sustain) {
        v->env_level = v->env_sustain;
        v->env_state = AUDIO_ENV_SUSTAIN;
        return;
    }
    v->env_state = AUDIO_ENV_DECAY;
    v->env_rate = (v->env_level - v->env_sustain) / (int32_t)df;
    if (v->env_rate < 1) {
        v->env_rate = 1;
    }
}

static void env_enter_release(audio_voice_t *v, const audio_instrument_t *ins) {
    uint32_t rf = (uint32_t)ms_to_frames(ins->release_ms);
    v->env_state = AUDIO_ENV_RELEASE;
    if (rf == 0 || v->env_level <= 0) {
        v->env_level = 0;
        v->env_state = AUDIO_ENV_OFF;
        v->active = 0;
        return;
    }
    v->env_rate = v->env_level / (int32_t)rf;
    if (v->env_rate < 1) {
        v->env_rate = 1;
    }
}

static void voice_start_tone(audio_voice_t *v, const audio_instrument_t *ins,
                             int note, uint32_t hold) {
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->source = 0;
    v->note = (uint8_t)note;
    v->inc = s_note_inc[note & 0x7f];
    v->lfsr = (uint16_t)(0x4001 | (note << 4));
    v->hold_frames = hold;
    v->env_sustain = (int32_t)ins->sustain * 256;

    uint32_t af = (uint32_t)ms_to_frames(ins->attack_ms);
    if (af == 0) {
        v->env_level = 65536;
        env_enter_decay(v, ins);
    } else {
        v->env_state = AUDIO_ENV_ATTACK;
        v->env_rate = 65536 / (int32_t)af;
        if (v->env_rate < 1) {
            v->env_rate = 1;
        }
    }
}

static void voice_start_sample(audio_voice_t *v, const audio_pcm_t *smp,
                               int note, uint32_t hold) {
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->source = 1;
    v->note = (uint8_t)note;
    v->step = ((uint64_t)smp->rate * s_pitch_q32[note & 0x7f]) / AUDIO_SAMPLE_RATE;
    v->hold_frames = hold;
    v->env_sustain = 65536;
    v->env_level = 65536;
    v->env_state = AUDIO_ENV_SUSTAIN;
}

static void voice_apply_mix(audio_voice_t *v, int volume, int pan) {
    if (volume < 0) volume = 0;
    if (volume > 255) volume = 255;
    if (pan < -64) pan = -64;
    if (pan > 63) pan = 63;
    v->volume = (uint8_t)volume;
    int lg = pan >= 63 ? 0 : 255 - (pan > 0 ? pan * 4 : 0);
    int rg = pan <= -64 ? 0 : 255 + (pan < 0 ? pan * 4 : 0);
    v->lgain = (uint8_t)(lg < 0 ? 0 : (lg > 255 ? 255 : lg));
    v->rgain = (uint8_t)(rg < 0 ? 0 : (rg > 255 ? 255 : rg));
}

/* Trigger `sound` into voice `idx`. dur_ms 0 = one-shot envelope. */
static void trigger_voice(audio_state_t *a, int idx, int sound, int note,
                          int volume, int pan, int dur_ms) {
    if (idx < 0 || idx >= AUDIO_VOICES || !audio_sound_defined(a, sound)) {
        return;
    }
    audio_voice_t *v = &a->voices[idx];
    if (sound < AUDIO_INSTRUMENT_MAX) {
        const audio_instrument_t *ins = &a->instruments[sound];
        voice_start_tone(v, ins, note, oneshot_hold(ins, (uint32_t)dur_ms));
        v->sound = (uint8_t)sound;
        voice_apply_mix(v, (ins->volume * volume) >> 8, pan);
    } else {
        const audio_pcm_t *smp = &a->samples[sound - AUDIO_INSTRUMENT_MAX];
        uint32_t hold = dur_ms > 0 ? (uint32_t)ms_to_frames((uint32_t)dur_ms)
                                   : 0xFFFFFFFFu; /* until sample end */
        voice_start_sample(v, smp, note, hold);
        v->sound = (uint8_t)sound;
        voice_apply_mix(v, (s_sample_envelope.volume * volume) >> 8, pan);
    }
}

/* ------------------------------------------------------------------ */
/* Request servicing + score scheduling (producer side)                */
/* ------------------------------------------------------------------ */

static void score_stop_voices(audio_state_t *a) {
    for (int c = 0; c < AUDIO_CHANNELS; c++) {
        a->voices[c].active = 0;
        a->voices[c].env_state = AUDIO_ENV_OFF;
    }
}

static void schedule_block(audio_state_t *a, int n) {
    const audio_score_t *sc = &a->scores[a->score_index];
    uint32_t block_end = a->score_frame + (uint32_t)n;
    for (int c = 0; c < AUDIO_CHANNEL_MAX; c++) {
        const audio_channel_t *ch = &sc->ch[c];
        uint16_t i = a->ch_next[c];
        while (i < ch->count) {
            const audio_event_t *ev = &a->events[ch->start + i];
            if (ms_to_frames(ev->time_ms) >= block_end) {
                break;
            }
            trigger_voice(a, c, ev->sound, ev->note, ev->volume, ev->pan,
                          (int)ev->dur_ms);
            i++;
        }
        a->ch_next[c] = i;
    }
}

static void apply_requests(audio_state_t *a) {
    if (a->req_stop) {
        a->req_stop = 0;
        a->score_active = 0;
        score_stop_voices(a);
    }
    if (a->req_play) {
        a->req_play = 0;
        if (a->req_score < AUDIO_SCORE_MAX && a->scores[a->req_score].valid) {
            score_stop_voices(a);
            a->score_index = a->req_score;
            a->score_loop = a->req_loop;
            a->score_frame = 0;
            for (int c = 0; c < AUDIO_CHANNEL_MAX; c++) {
                a->ch_next[c] = 0;
            }
            a->score_active = 1;
        }
    }
    for (int s = 0; s < AUDIO_ONESHOT_VOICES; s++) {
        if (a->stop_flags[s]) {
            a->stop_flags[s] = 0;
            audio_voice_t *v = &a->voices[AUDIO_CHANNELS + s];
            v->active = 0;
            v->env_state = AUDIO_ENV_OFF;
        }
    }
    unsigned tail = atomic_load_explicit(&a->trig_tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&a->trig_head, memory_order_acquire);
    while (tail != head) {
        const audio_trigger_t *t = &a->trig[tail];
        trigger_voice(a, AUDIO_CHANNELS + (int)tail, t->sound, t->note, t->volume,
                      t->pan, t->dur_ms);
        tail = (tail + 1) % AUDIO_ONESHOT_VOICES;
    }
    atomic_store_explicit(&a->trig_tail, tail, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* Mixer                                                               */
/* ------------------------------------------------------------------ */

static inline int32_t tone_sample(const audio_voice_t *v,
                                  const audio_instrument_t *ins) {
    uint32_t p16 = v->phase >> 16;
    switch (ins->wave) {
    case AUDIO_WAVE_SQUARE:
        return p16 < (uint32_t)ins->duty * 4096u ? 32767 : -32768;
    case AUDIO_WAVE_TRIANGLE: {
        uint32_t idx = v->phase >> 24; /* 0..255 */
        int32_t s = idx < 128 ? (int32_t)idx * 2 - 128 : (int32_t)(255 - idx) * 2 - 128;
        return s * 256;
    }
    case AUDIO_WAVE_SAW:
        return (int32_t)p16 - 32768;
    case AUDIO_WAVE_SINE:
    default:
        return s_sine[v->phase >> 24];
    }
}

/* Advance the noise LFSR once per waveform period. */
static inline int32_t noise_sample(audio_voice_t *v) {
    uint32_t before = v->phase;
    v->phase += v->inc;
    if (v->phase < before) {
        uint16_t l = v->lfsr;
        v->lfsr = (uint16_t)((l >> 1) | (((l ^ (l >> 1)) & 1) << 14));
    }
    return (v->lfsr & 1) ? 32767 : -32768;
}

static inline void mix_frame(audio_state_t *a, int16_t *out) {
    int32_t l = 0, r = 0;
    for (int i = 0; i < AUDIO_VOICES; i++) {
        audio_voice_t *v = &a->voices[i];
        if (!v->active) {
            continue;
        }

        int32_t e_l, e_r;
        if (v->source == 0) {
            const audio_instrument_t *inst = &a->instruments[v->sound];
            int32_t s;
            if (inst->wave == AUDIO_WAVE_NOISE) {
                s = noise_sample(v);
            } else {
                s = tone_sample(v, inst);
                v->phase += v->inc;
            }
            e_l = e_r = (s * v->env_level) >> 16;
        } else {
            const audio_pcm_t *smp = &a->samples[v->sound - AUDIO_INSTRUMENT_MAX];
            uint64_t end = (uint64_t)smp->frames << 32;
            if (v->pos >= end) {
                v->active = 0;
                v->env_state = AUDIO_ENV_OFF;
                continue;
            }
            uint32_t idx = (uint32_t)(v->pos >> 32);
            uint32_t frac = (uint32_t)(v->pos & 0xffffffffu);
            const int16_t *base =
                a->sample_pool + smp->offset / 2 + (size_t)idx * smp->channels;
            int32_t c0 = base[0];
            int32_t c1 = (idx + 1 < smp->frames) ? base[smp->channels] : c0;
            int32_t s0 = c0 + (int32_t)(((int64_t)(c1 - c0) * frac) >> 32);
            if (smp->channels == 2) {
                int32_t d0 = base[1];
                int32_t d1 = (idx + 1 < smp->frames) ? base[3] : d0;
                int32_t s1 = d0 + (int32_t)(((int64_t)(d1 - d0) * frac) >> 32);
                e_l = (s0 * v->env_level) >> 16;
                e_r = (s1 * v->env_level) >> 16;
            } else {
                e_l = e_r = (s0 * v->env_level) >> 16;
            }
            v->pos += v->step;
        }

        e_l = (e_l * v->volume) >> 8;
        e_r = (e_r * v->volume) >> 8;
        l += (e_l * v->lgain) >> 8;
        r += (e_r * v->rgain) >> 8;

        /* Envelope + hold timer. */
        const audio_instrument_t *ins =
            &a->instruments[v->source == 0 ? v->sound : 0];
        if (v->source == 1) {
            ins = &s_sample_envelope;
        }
        if (v->hold_frames != 0xFFFFFFFFu && v->env_state != AUDIO_ENV_OFF) {
            if (v->hold_frames > 0) {
                v->hold_frames--;
            }
            if (v->hold_frames == 0 && v->env_state != AUDIO_ENV_RELEASE) {
                env_enter_release(v, ins);
            }
        }
        switch (v->env_state) {
        case AUDIO_ENV_ATTACK:
            v->env_level += v->env_rate;
            if (v->env_level >= 65536) {
                v->env_level = 65536;
                env_enter_decay(v, ins);
            }
            break;
        case AUDIO_ENV_DECAY:
            v->env_level -= v->env_rate;
            if (v->env_level <= v->env_sustain) {
                v->env_level = v->env_sustain;
                v->env_state = AUDIO_ENV_SUSTAIN;
            }
            break;
        case AUDIO_ENV_RELEASE:
            v->env_level -= v->env_rate;
            if (v->env_level <= 0) {
                v->env_level = 0;
                v->env_state = AUDIO_ENV_OFF;
                v->active = 0;
            }
            break;
        default:
            break;
        }
    }

    int32_t master = a->master;
    l = (l * master) >> 8;
    r = (r * master) >> 8;
    if (l > 32767) l = 32767;
    if (l < -32768) l = -32768;
    if (r > 32767) r = 32767;
    if (r < -32768) r = -32768;
    out[0] = (int16_t)l;
    out[1] = (int16_t)r;
}

void audio_mix(audio_state_t *a, int16_t *out, int frames) {
    if (!a || frames <= 0) {
        if (out && frames > 0) {
            memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));
        }
        return;
    }
    apply_requests(a);

    int done = 0;
    while (done < frames) {
        int n = frames - done;
        if (n > AUDIO_BLOCK) {
            n = AUDIO_BLOCK;
        }
        if (a->score_active) {
            schedule_block(a, n);
        }
        for (int i = 0; i < n; i++) {
            mix_frame(a, out + (size_t)(done + i) * 2);
        }
        if (a->score_active) {
            a->score_frame += (uint32_t)n;
            const audio_score_t *sc = &a->scores[a->score_index];
            uint32_t len = (uint32_t)ms_to_frames(sc->length_ms);
            if (a->score_frame >= len) {
                if (a->score_loop && len > 0) {
                    a->score_frame -= len;
                    for (int c = 0; c < AUDIO_CHANNEL_MAX; c++) {
                        a->ch_next[c] = 0;
                    }
                } else {
                    a->score_active = 0;
                }
            }
        }
        done += n;
    }
}
