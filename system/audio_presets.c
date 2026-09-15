/* audio_presets.c
 *
 * The built-in sound bank: instruments for tunes and effects for games,
 * baked into the firmware like the ROM font, so a program can play
 * "laser" or write a track for "piano" without defining anything.
 * Played by name (SoundPlay("coin")) or by id AUDIO_PRESET_BASE + index.
 *
 * Each entry is an ordinary instrument definition (audio.h): a waveform,
 * an ADSR envelope and the pitch/tone effects. Instruments are tuned to
 * be played at any note; effects carry their own note and are usually
 * played as they are. Keep the list append-only: ids are the index.
 */
#include <string.h>

#include "audio.h"

#define OPEN AUDIO_CUTOFF_OPEN

/* Waveform, duty, attack, decay, sustain, release, volume, note,
 * slide, vib_depth, vib_rate, arp, arp2, arp_ms, arp_loop, cutoff. */
#define INS(wave_, duty_, a, d, s, r, vol, note_, slide_, vd, vr, arp_, arp2_, arpms, arploop, cut) \
    {1, wave_, duty_, a, d, s, r, vol, note_, slide_, vd, vr, arp_, arp2_, arpms, arploop, cut}

static const audio_preset_t s_presets[] = {
    /* ---- instruments (play at any note) ---- */
    {"lead",    0, INS(AUDIO_WAVE_SQUARE,   8,  2,  20, 200,  40, 200, 0,    0, 0,  0,  0, 0, 0, 0, OPEN)},
    {"pulse",   0, INS(AUDIO_WAVE_SQUARE,   4,  2,  30, 160,  60, 200, 0,    0, 8, 60,  0, 0, 0, 0, OPEN)},
    {"bass",    0, INS(AUDIO_WAVE_TRIANGLE, 8,  2,  60, 180,  60, 255, 0,    0, 0,  0,  0, 0, 0, 0, 120)},
    {"piano",   0, INS(AUDIO_WAVE_SAW,      8,  1, 220,  50, 120, 210, 0,    0, 0,  0,  0, 0, 0, 0, 140)},
    {"organ",   0, INS(AUDIO_WAVE_SINE,     8, 20,  20, 240,  80, 230, 0,    0, 6, 55,  0, 0, 0, 0, OPEN)},
    {"strings", 0, INS(AUDIO_WAVE_SAW,      8,120,  60, 200, 200, 170, 0,    0,12, 50,  0, 0, 0, 0, 110)},
    {"flute",   0, INS(AUDIO_WAVE_SINE,     8, 40,  30, 220, 100, 240, 0,    0,15, 52,  0, 0, 0, 0, OPEN)},
    {"brass",   0, INS(AUDIO_WAVE_SAW,      8, 30,  40, 210,  90, 200, 0,    0, 5, 45,  0, 0, 0, 0, 170)},
    {"bell",    0, INS(AUDIO_WAVE_SINE,     8,  1, 250,  30, 250, 220, 0,    0, 0,  0, 12, 0, 255, 0, OPEN)},
    {"pluck",   0, INS(AUDIO_WAVE_SQUARE,   6,  1,  90,   0, 120, 220, 0,    0, 0,  0,  0, 0, 0, 0, 160)},
    {"chime",   0, INS(AUDIO_WAVE_TRIANGLE, 8,  1, 200,  40, 250, 220, 0,    0, 0,  0,  0, 0, 0, 0, OPEN)},
    {"kick",    0, INS(AUDIO_WAVE_SINE,     8,  1,  90,   0,  40, 255, 43, -400, 0,  0,  0, 0, 0, 0, OPEN)},
    {"snare",   0, INS(AUDIO_WAVE_NOISE,    8,  1,  80,   0,  60, 220, 72,    0, 0,  0,  0, 0, 0, 0, 200)},
    {"hihat",   0, INS(AUDIO_WAVE_NOISE,    8,  1,  25,   0,  20, 160, 96,    0, 0,  0,  0, 0, 0, 0, OPEN)},
    {"tom",     0, INS(AUDIO_WAVE_TRIANGLE, 8,  1, 120,   0,  60, 255, 50, -120, 0,  0,  0, 0, 0, 0, OPEN)},
    {"clap",    0, INS(AUDIO_WAVE_NOISE,    8,  1,  60,   0,  40, 200, 80,    0, 0,  0,  0, 0, 0, 0, 150)},
    /* ---- effects (play as they are; note = their own pitch) ---- */
    {"coin",      1, INS(AUDIO_WAVE_SQUARE,   8,  1,  60, 120,  80, 220, 84,    0, 0,  0,  7, 0,  60, 0, OPEN)},
    {"jump",      1, INS(AUDIO_WAVE_SQUARE,   6,  1, 120,   0,  40, 200, 60,  180, 0,  0,  0, 0,   0, 0, OPEN)},
    {"laser",     1, INS(AUDIO_WAVE_SQUARE,   4,  1, 140,   0,  30, 200, 96, -300, 0,  0,  0, 0,   0, 0, OPEN)},
    {"zap",       1, INS(AUDIO_WAVE_SAW,      8,  1,  80,   0,  20, 200, 90, -600, 0,  0,  0, 0,   0, 0, OPEN)},
    {"explosion", 1, INS(AUDIO_WAVE_NOISE,    8,  5, 250, 120, 250, 255, 40, -200, 0,  0,  0, 0,   0, 0, 90)},
    {"hit",       1, INS(AUDIO_WAVE_NOISE,    8,  1,  60,   0,  30, 230, 55,    0, 0,  0,  0, 0,   0, 0, 120)},
    {"hurt",      1, INS(AUDIO_WAVE_SAW,      8,  1, 120,   0,  60, 220, 64, -100, 0,  0,  0, 0,   0, 0, 180)},
    {"powerup",   1, INS(AUDIO_WAVE_SQUARE,   8,  1, 200,  60, 100, 220, 60,    0, 0,  0,  4, 7,  45, 1, OPEN)},
    {"blip",      1, INS(AUDIO_WAVE_SQUARE,   8,  1,  30,   0,  10, 200, 84,    0, 0,  0,  0, 0,   0, 0, OPEN)},
    {"select",    1, INS(AUDIO_WAVE_SQUARE,   8,  1,  40,  60,  30, 200, 76,    0, 0,  0, 12, 0,  30, 0, OPEN)},
    {"error",     1, INS(AUDIO_WAVE_SQUARE,   8,  1, 100,  80,  60, 220, 45,    0, 0,  0, -3, 0,  90, 0, OPEN)},
    {"alarm",     1, INS(AUDIO_WAVE_SQUARE,   8, 10,  20, 220,  40, 220, 72,    0, 0,  0,  5, 0, 120, 1, OPEN)},
    {"engine",    1, INS(AUDIO_WAVE_SAW,      8, 30,  30, 200,  60, 200, 36,    0,20, 60,  0, 0,   0, 0, 60)},
    {"splash",    1, INS(AUDIO_WAVE_NOISE,    8,  5, 200,  60, 200, 200, 66,  -80, 0,  0,  0, 0,   0, 0, 70)},
    {"bounce",    1, INS(AUDIO_WAVE_TRIANGLE, 8,  1,  80,   0,  30, 230, 62,  120, 0,  0,  0, 0,   0, 0, OPEN)},
    {"teleport",  1, INS(AUDIO_WAVE_SQUARE,   8,  5, 250, 140, 100, 200, 60,    0,60, 90, 12, 0,  25, 1, OPEN)},
};

int audio_preset_count(void) {
    return (int)(sizeof(s_presets) / sizeof(s_presets[0]));
}

const audio_preset_t *audio_preset(int index) {
    if (index < 0 || index >= audio_preset_count()) {
        return NULL;
    }
    return &s_presets[index];
}

int audio_preset_find(const char *name) {
    if (!name) {
        return -1;
    }
    for (int i = 0; i < audio_preset_count(); i++) {
        if (strcmp(s_presets[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}
