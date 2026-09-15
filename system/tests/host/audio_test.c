/* audio_test.c
 *
 * Host-side tests for the Phase 8 audio pieces: the synth engine
 * (oscillators, envelopes, panning, mixing), the score scheduler
 * (timing, release, loop wrap), PCM sample playback, and the Lua
 * sound module (SoundDefine/MusicDefine/... + WAV loading over the
 * mock SD card).
 *
 * Build and run:
 *   cmake -S tests/host -B build-host
 *   cmake --build build-host
 *   ./build-host/spicomputer_audio_tests
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "fs_core0.h"
#include "lauxlib.h"
#include "lua.h"
#include "program.h"
#include "rpc.h"
#include "system_state.h"

extern void mock_set_file(const char *path, const char *content);
extern void mock_set_file_bytes(const char *path, const void *data, size_t len);

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL: %s (line %d)\n", msg, __LINE__);                   \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void rpc_wait_host(void) {
    fs_core0_service();
}

static void define_square(audio_state_t *a, int id, int release_ms) {
    audio_instrument_t *ins = &a->instruments[id];
    memset(ins, 0, sizeof(*ins));
    ins->defined = 1;
    ins->wave = AUDIO_WAVE_SQUARE;
    ins->duty = 8;
    ins->attack_ms = 0;
    ins->decay_ms = 0;
    ins->sustain = 255;
    ins->release_ms = (uint8_t)release_ms;
    ins->volume = 255;
}

static void add_event(audio_state_t *a, audio_score_t *sc, int ch, int idx,
                      uint32_t at, uint32_t dur, int note) {
    audio_event_t *ev = &a->events[sc->ch[ch].start + idx];
    memset(ev, 0, sizeof(*ev));
    ev->time_ms = at;
    ev->dur_ms = dur;
    ev->sound = 0;
    ev->note = (uint8_t)note;
    ev->volume = 255;
    ev->pan = 0;
}

/* ---------------- test 1: note names ---------------- */

static void test_note_parse(void) {
    CHECK(audio_note_parse("C4") == 60, "C4 = 60");
    CHECK(audio_note_parse("A4") == 69, "A4 = 69");
    CHECK(audio_note_parse("C#4") == 61, "C#4 = 61");
    CHECK(audio_note_parse("Db4") == 61, "Db4 = 61");
    CHECK(audio_note_parse("C-1") == 0, "C-1 = 0");
    CHECK(audio_note_parse("G9") == 127, "G9 = 127");
    CHECK(audio_note_parse("H4") == -1, "H4 invalid");
    CHECK(audio_note_parse("C") == -1, "C invalid");
    CHECK(audio_note_parse("") == -1, "empty invalid");
}

/* ---------------- test 2: square + stereo ---------------- */

static void test_square(void) {
    audio_state_t a;
    audio_state_init(&a);
    define_square(&a, 0, 10);
    int slot = audio_trigger(&a, 0, 69 /* A4 */, 255, 0, 1000);
    CHECK(slot == 0, "one-shot voice slot 0");

    static int16_t buf[512 * 2];
    audio_mix(&a, buf, 512);
    CHECK(a.voices[AUDIO_CHANNELS].active, "voice still sounding");
    CHECK(buf[0] > 30000, "square starts positive");
    CHECK(buf[1] == buf[0], "center pan: left == right");

    int crossings = 0;
    for (int i = 1; i < 512; i++) {
        int a0 = buf[(i - 1) * 2] > 0, a1 = buf[i * 2] > 0;
        if (a0 != a1) {
            crossings++;
        }
    }
    /* A4 = 440 Hz: 512 frames = 5.1 periods -> ~10 half-period flips. */
    CHECK(crossings >= 8 && crossings <= 12, "A4 square frequency");
}

/* ---------------- test 3: envelope + one-shot end ---------------- */

static void test_envelope(void) {
    audio_state_t a;
    audio_state_init(&a);
    audio_instrument_t *ins = &a.instruments[0];
    define_square(&a, 0, 10);
    ins->attack_ms = 10;
    ins->decay_ms = 10;
    ins->sustain = 128;

    audio_trigger(&a, 0, 60, 255, 0, 100); /* 100 ms hold */
    audio_voice_t *v = &a.voices[AUDIO_CHANNELS];

    static int16_t buf[2048 * 2];
    audio_mix(&a, buf, 100);
    int32_t early = v->env_level;
    CHECK(early > 0 && early < 65536, "attack in progress");
    audio_mix(&a, buf, 300);
    CHECK(v->env_level > early, "attack rising");
    audio_mix(&a, buf, 500); /* total 900: past attack+decay */
    CHECK(v->env_state == AUDIO_ENV_SUSTAIN, "reaches sustain");
    CHECK(v->env_level == 128 * 256, "sustain level");
    audio_mix(&a, buf, 200); /* total 1100, still inside the hold */
    CHECK(v->env_state == AUDIO_ENV_SUSTAIN, "holds sustain until dur");
    audio_mix(&a, buf, 3510); /* total 4410 = 100 ms hold: release starts */
    CHECK(v->env_state == AUDIO_ENV_RELEASE, "release after hold");
    audio_mix(&a, buf, 500);
    CHECK(!v->active, "one-shot ends after release");
}

/* ---------------- test 4: score scheduling ---------------- */

static void test_score_schedule(void) {
    audio_state_t a;
    audio_state_init(&a);
    define_square(&a, 0, 10);

    audio_score_t *sc = &a.scores[0];
    memset(sc, 0, sizeof(*sc));
    sc->valid = 1;
    sc->length_ms = 150;
    sc->ch[0].start = 0;
    sc->ch[0].count = 2;
    a.events_used = 2;
    add_event(&a, sc, 0, 0, 0, 50, 60);
    add_event(&a, sc, 0, 1, 100, 50, 60);

    audio_play_score(&a, 0, 0);
    static int16_t buf[4410 * 2];

    /* First 100 ms: note 1 sounds, silence between 60 and 100 ms. */
    audio_mix(&a, buf, 4410);
    CHECK(buf[10 * 2] != 0, "note 1 starts at 0");
    CHECK(buf[3000 * 2] == 0, "silent after note 1 release");
    CHECK(audio_score_playing(&a), "score still playing");

    /* Event 2 is due at the 100 ms boundary. */
    audio_mix(&a, buf, 4410);
    CHECK(buf[10 * 2] != 0, "note 2 starts at 100 ms");

    /* Score length 150 ms: finished after two more blocks (200 ms). */
    audio_mix(&a, buf, 4410);
    audio_mix(&a, buf, 4410);
    CHECK(!audio_score_playing(&a), "score ends at its length");
}

/* ---------------- test 5: loop ---------------- */

static void test_loop(void) {
    audio_state_t a;
    audio_state_init(&a);
    define_square(&a, 0, 0); /* immediate stop on release */

    audio_score_t *sc = &a.scores[0];
    memset(sc, 0, sizeof(*sc));
    sc->valid = 1;
    sc->length_ms = 100;
    sc->ch[0].start = 0;
    sc->ch[0].count = 1;
    a.events_used = 1;
    add_event(&a, sc, 0, 0, 0, 50, 60);

    audio_play_score(&a, 0, 1);
    static int16_t buf[4410 * 2];

    audio_mix(&a, buf, 4410);
    CHECK(buf[10 * 2] != 0, "loop pass 1 sounds");
    CHECK(audio_score_playing(&a), "loop still playing after pass 1");
    audio_mix(&a, buf, 4410);
    CHECK(buf[10 * 2] != 0, "loop pass 2 retriggers");
    CHECK(audio_score_playing(&a), "loop still playing after pass 2");
    audio_mix(&a, buf, 4410);
    CHECK(audio_score_playing(&a), "loop keeps playing");
}

/* ---------------- test 6: pan + master volume ---------------- */

static void test_pan_volume(void) {
    audio_state_t a;
    audio_state_init(&a);
    define_square(&a, 0, 0);
    static int16_t buf[64 * 2];

    audio_trigger(&a, 0, 69, 255, -64, 1000);
    audio_mix(&a, buf, 16);
    CHECK(buf[0] != 0 && buf[1] == 0, "hard left: right silent");
    audio_pause(&a);

    audio_trigger(&a, 0, 69, 255, 63, 1000);
    audio_mix(&a, buf, 16);
    CHECK(buf[0] == 0 && buf[1] != 0, "hard right: left silent");
    audio_pause(&a);

    a.master = 255;
    audio_trigger(&a, 0, 69, 255, 0, 1000);
    audio_mix(&a, buf, 8);
    int32_t loud = buf[0];
    audio_pause(&a);

    a.master = 128;
    audio_trigger(&a, 0, 69, 255, 0, 1000);
    audio_mix(&a, buf, 8);
    int32_t half = buf[0];
    CHECK(loud > 30000, "full volume sample");
    CHECK(abs((int)(loud / 2 - half)) < 200, "master volume scales output");
}

/* ---------------- test 7: PCM sample playback ---------------- */

static void test_sample(void) {
    audio_state_t a;
    audio_state_init(&a);
    a.sample_pool = (int16_t *)malloc(AUDIO_SAMPLE_POOL_MAX);
    CHECK(a.sample_pool != NULL, "sample pool allocated");
    int16_t *pool = a.sample_pool;
    for (int i = 0; i < 8; i++) {
        pool[i] = (int16_t)((i + 1) * 1000);
    }
    a.samples[0].defined = 1;
    a.samples[0].channels = 1;
    a.samples[0].rate = 44100;
    a.samples[0].frames = 8;
    a.samples[0].offset = 0;
    a.sample_pool_used = 16;

    int slot = audio_trigger(&a, AUDIO_INSTRUMENT_MAX, 60 /* C4 */, 255, 0, 0);
    CHECK(slot == 0, "sample trigger accepted");
    static int16_t buf[32 * 2];
    audio_mix(&a, buf, 16);
    CHECK(buf[0] != 0, "sample output nonzero");
    CHECK(a.voices[AUDIO_CHANNELS].pos == ((uint64_t)8 << 32),
          "sample plays to the end then stops");
    CHECK(!a.voices[AUDIO_CHANNELS].active, "sample voice finished");

    /* A root note: a recording of C2 (36) plays unshifted at C2 and an
     * octave faster at C3; without a root C4 is unshifted. */
    a.samples[0].root = 36;
    int s1 = audio_trigger(&a, AUDIO_INSTRUMENT_MAX, 36, 255, 0, 0);
    audio_mix(&a, buf, 1);
    CHECK(a.voices[AUDIO_CHANNELS + s1].step == ((uint64_t)1 << 32), "root note plays at the recorded rate");
    audio_stop_all_voices(&a);
    audio_mix(&a, buf, 1);
    int s2 = audio_trigger(&a, AUDIO_INSTRUMENT_MAX, 48, 255, 0, 0);
    audio_mix(&a, buf, 1);
    CHECK(a.voices[AUDIO_CHANNELS + s2].step == ((uint64_t)2 << 32), "an octave above the root doubles the rate");
    audio_stop_all_voices(&a);
    audio_mix(&a, buf, 1);
    audio_state_free(&a);
}

/* ---------------- test 8: Lua sound module ---------------- */

static const char *SOUND_LUA_SRC =
    "results = {}\n"
    "function setup()\n"
    "  results.define = tostring(SoundDefine(0, {wave='square', attack=0,"
    " decay=0, sustain=255, release=10}))\n"
    "  local ok, err = pcall(SoundDefine, 99, {})\n"
    "  results.define_bad = tostring(err)\n"
    "  local ok2, err2 = pcall(SoundDefine, 1, {wave='bogus'})\n"
    "  results.wave_bad = tostring(err2)\n"
    "  results.music = tostring(MusicDefine('tune', {loop=false, channels={"
    "{ {at=0,sound=0,note='C4',dur=50}, {at=100,sound=0,note='E4',dur=50} }}}))\n"
    "  results.music_play = tostring(MusicPlay('tune'))\n"
    "  results.playing = tostring(MusicPlaying())\n"
    "  local mok, merr = MusicPlay('nope')\n"
    "  results.play_bad = tostring(merr)\n"
    "  local pv, perr = SoundPlay(1, 'C4')\n"
    "  results.sound_bad = tostring(perr)\n"
    "  results.voice = tostring(SoundPlay(0, 'A4', 50))\n"
    "  results.volume = tostring(SoundVolume(200))\n"
    "  results.wavid = tostring(SoundLoad('beep.wav'))\n"
    "  results.wavplay = tostring(SoundPlay(32, 'C4', 0))\n"
    "end\n"
    "function tick() end\n";

static void push_wav(int16_t *samples, int n) {
    /* 16-bit mono WAV writer for the mock SD. */
    static uint8_t buf[256];
    int data_bytes = n * 2;
    uint32_t riff_size = 36 + (uint32_t)data_bytes;
    memcpy(buf, "RIFF", 4);
    buf[4] = (uint8_t)riff_size;
    buf[5] = (uint8_t)(riff_size >> 8);
    buf[6] = (uint8_t)(riff_size >> 16);
    buf[7] = (uint8_t)(riff_size >> 24);
    memcpy(buf + 8, "WAVEfmt ", 8);
    buf[16] = 16; /* fmt chunk size */
    buf[20] = 1;  /* PCM */
    buf[22] = 1;  /* mono */
    buf[24] = 0x44; buf[25] = 0xac; buf[26] = 0; buf[27] = 0;   /* 44100 */
    buf[28] = 0x88; buf[29] = 0x58; buf[30] = 1; buf[31] = 0;   /* byte rate */
    buf[32] = 2; buf[33] = 0;  /* block align */
    buf[34] = 16; buf[35] = 0; /* bits */
    memcpy(buf + 36, "data", 4);
    buf[40] = (uint8_t)data_bytes;
    buf[41] = (uint8_t)(data_bytes >> 8);
    buf[42] = 0;
    buf[43] = 0;
    for (int i = 0; i < n; i++) {
        buf[44 + i * 2] = (uint8_t)(samples[i] & 0xff);
        buf[45 + i * 2] = (uint8_t)((samples[i] >> 8) & 0xff);
    }
    mock_set_file_bytes("beep.wav", buf, (size_t)(44 + data_bytes));
}

static const char *lua_global_string(lua_State *L, const char *field) {
    lua_getglobal(L, "results");
    lua_getfield(L, -1, field);
    const char *s = lua_tostring(L, -1);
    static char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", s ? s : "<nil>");
    lua_pop(L, 2);
    return tmp;
}

/* ---- pitch and tone effects, and the built-in bank ---- */

/* Zero crossings of the left channel over `frames` frames: a frequency
 * estimate that is independent of the waveform. */
static int crossings_in(const int16_t *buf, int frames) {
    int n = 0;
    for (int i = 1; i < frames; i++) {
        if ((buf[(i - 1) * 2] < 0) != (buf[i * 2] < 0)) n++;
    }
    return n;
}

static void test_effects(void) {
    audio_state_t a;
    static int16_t buf[AUDIO_SAMPLE_RATE * 2];
    audio_state_init(&a);

    /* A slide: the same note gets higher over time. */
    a.instruments[0] = (audio_instrument_t){.defined = 1, .wave = AUDIO_WAVE_SQUARE,
        .duty = 8, .sustain = 255, .volume = 255, .slide = 12};
    audio_trigger(&a, 0, 57, 255, 0, 900); /* A3, +12 semitones a second */
    audio_mix(&a, buf, 4410);              /* 0..100 ms */
    int early = crossings_in(buf, 4410);
    audio_mix(&a, buf, 4410 * 6);          /* ..700 ms */
    audio_mix(&a, buf, 4410);              /* 700..800 ms */
    int late = crossings_in(buf, 4410);
    CHECK(early >= 40 && early <= 50, "slide starts near A3 (44 crossings per 100 ms)");
    CHECK(late > early * 3 / 2, "slide raised the pitch");
    audio_stop_all_voices(&a);
    audio_mix(&a, buf, 64);

    /* An arpeggio step: up 12 semitones after 50 ms, held. */
    a.instruments[1] = (audio_instrument_t){.defined = 1, .wave = AUDIO_WAVE_SQUARE,
        .duty = 8, .sustain = 255, .volume = 255, .arp = 12, .arp_ms = 50};
    audio_trigger(&a, 1, 57, 255, 0, 600);
    audio_mix(&a, buf, 2205); /* 0..50 ms */
    int before = crossings_in(buf, 2205);
    audio_mix(&a, buf, 2205);
    audio_mix(&a, buf, 2205); /* 100..150 ms */
    int after = crossings_in(buf, 2205);
    CHECK(after > before * 17 / 10 && after < before * 23 / 10, "arpeggio doubles the pitch");
    audio_stop_all_voices(&a);
    audio_mix(&a, buf, 64);

    /* Vibrato: the pitch wobbles, so per-block crossing counts vary. */
    a.instruments[2] = (audio_instrument_t){.defined = 1, .wave = AUDIO_WAVE_SQUARE,
        .duty = 8, .sustain = 255, .volume = 255, .vib_depth = 200, .vib_rate = 50};
    audio_trigger(&a, 2, 69, 255, 0, 600);
    int lo = 100000, hi = 0;
    for (int i = 0; i < 20; i++) {
        audio_mix(&a, buf, 2205);
        int c = crossings_in(buf, 2205);
        if (c < lo) lo = c;
        if (c > hi) hi = c;
    }
    CHECK(hi - lo >= 4, "vibrato varies the pitch");
    audio_stop_all_voices(&a);
    audio_mix(&a, buf, 64);

    /* Cutoff: a filtered square has smaller sample-to-sample steps. */
    a.instruments[3] = (audio_instrument_t){.defined = 1, .wave = AUDIO_WAVE_SQUARE,
        .duty = 8, .sustain = 255, .volume = 255, .cutoff = 40};
    audio_trigger(&a, 3, 57, 255, 0, 300);
    audio_mix(&a, buf, 4410);
    int max_step = 0;
    for (int i = 1; i < 4410; i++) {
        int d = abs(buf[i * 2] - buf[(i - 1) * 2]);
        if (d > max_step) max_step = d;
    }
    CHECK(max_step < 20000, "low cutoff rounds off the square's edges");
    CHECK(buf[4000 * 2] != 0, "filtered voice still sounds");
    audio_stop_all_voices(&a);
    audio_mix(&a, buf, 64);
}

static void test_presets(void) {
    audio_state_t a;
    static int16_t buf[AUDIO_SAMPLE_RATE * 2];
    audio_state_init(&a);
    CHECK(audio_preset_count() >= 30, "a bank of at least 30 built-in sounds");
    CHECK(audio_preset_find("laser") >= 0 && audio_preset_find("piano") >= 0 &&
              audio_preset_find("nothing") < 0,
          "presets found by name");
    /* Every preset sounds when triggered and, as a one-shot, ends
     * within its envelope plus a second. */
    for (int i = 0; i < audio_preset_count(); i++) {
        const audio_preset_t *p = audio_preset(i);
        int id = AUDIO_PRESET_BASE + i;
        CHECK(audio_sound_defined(&a, id), "preset id is defined");
        int slot = audio_trigger(&a, id, p->ins.note ? p->ins.note : 60, 255, 0, 0);
        CHECK(slot >= 0, "preset triggers");
        audio_mix(&a, buf, 2205);
        int loud = 0;
        for (int k = 0; k < 2205; k++) {
            if (abs(buf[k * 2]) > loud) loud = abs(buf[k * 2]);
        }
        if (loud < 1000) {
            printf("FAIL: preset '%s' is silent in its first 50 ms\n", p->name);
            g_failures++;
        }
        uint32_t frames = 0;
        while (a.voices[AUDIO_CHANNELS + slot].active && frames < AUDIO_SAMPLE_RATE * 3) {
            audio_mix(&a, buf, 4410);
            frames += 4410;
        }
        if (a.voices[AUDIO_CHANNELS + slot].active) {
            printf("FAIL: preset '%s' does not end\n", p->name);
            g_failures++;
        }
    }
    /* The laser slides down: more crossings early than late. */
    int laser = AUDIO_PRESET_BASE + audio_preset_find("laser");
    audio_trigger(&a, laser, 96, 255, 0, 0);
    audio_mix(&a, buf, 1102);
    int early = crossings_in(buf, 1102);
    audio_mix(&a, buf, 1102);
    audio_mix(&a, buf, 1102);
    int late = crossings_in(buf, 1102);
    CHECK(late < early, "laser slides down");
    audio_stop_all_voices(&a);
    audio_mix(&a, buf, 64);
}

static const char *PRESET_LUA =
    "results = {}\n"
    "function setup()\n"
    "  local id, spec = SoundPreset('laser')\n"
    "  results.preset_id = tostring(id)\n"
    "  results.preset_wave = tostring(spec.wave) .. ':' .. tostring(spec.slide) .. ':' .. tostring(spec.note)\n"
    "  local _, err = SoundPreset('nothing')\n"
    "  results.preset_bad = tostring(err)\n"
    "  results.play_name = tostring(SoundPlay('coin'))\n"
    "  results.play_piano = tostring(SoundPlay('piano', 'E4', 200))\n"
    "  local ok, e2 = SoundPlay('nothing')\n"
    "  results.play_bad = tostring(e2)\n"
    "  results.define_base = tostring(SoundDefine(3, { base = 'laser', slide = -900, cutoff = 80 }))\n"
    "  local names = {}\n"
    "  for _, p in ipairs(SoundPresets()) do names[#names + 1] = p.name end\n"
    "  results.presets = #names .. ':' .. names[1]\n"
    "  results.music = tostring(MusicDefine('t', { channels = { { { at = 0, sound = 'kick' }, { at = 100, sound = 3, note = 'C5' } } } }))\n"
    "end\n"
    "function tick() end\n";

/* The Sound framework's Music.Track: the MML compiler, run from the
 * framework file itself (skipped when the IDE sources are not beside
 * the OS checkout). */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    buf[n] = 0;
    fclose(f);
    return buf;
}

static const char *MML_LUA =
    "results = {}\n"
    "function setup()\n"
    "  local ok, score = Music.Track('t', { tempo = 120, loop = true, channels = {\n"
    "    { sound = 'lead', mml = 'o4 l4 c d8 e8. r2 > c v8 c#2 [ e f ]2 c4&c8' },\n"
    "    { sound = 'kick', mml = 'l4 c c' } } })\n"
    "  results.ok = tostring(ok)\n"
    "  local ch = score.channels[1]\n"
    "  local parts = {}\n"
    "  for _, e in ipairs(ch) do parts[#parts + 1] = e.at .. '/' .. e.note .. '/' .. e.dur .. '/' .. e.vol end\n"
    "  results.ch1 = table.concat(parts, ' ')\n"
    "  results.ch2 = #score.channels[2] .. ':' .. score.channels[2][2].at .. ':' .. score.channels[2][1].note\n"
    "  results.loop = tostring(score.loop)\n"
    "  results.playing = tostring(MusicPlay('t') and MusicPlaying())\n"
    "end\n"
    "function tick() end\n";

static void test_mml(void) {
    char *sdk = read_file("../../../ide/macos/Sources/SPIIDECore/Resources/sdk/sound.lua");
    if (!sdk) {
        printf("skipped: MML test (no IDE checkout beside the OS)\n");
        return;
    }
    size_t n = strlen(sdk) + strlen(MML_LUA) + 2;
    char *prog = malloc(n);
    snprintf(prog, n, "%s\n%s", sdk, MML_LUA);
    mock_set_file("mml.lua", prog);
    CHECK(program_boot("mml.lua", NULL), "MML program boots");
    program_t *p = program_top();
    CHECK(p != NULL, "MML program running");
    if (p) {
        CHECK(strcmp(lua_global_string(p->L, "ok"), "true") == 0, "Music.Track defines the score");
        /* 120 bpm: a quarter is 500 ms, gate 7/8 = 437 ms; the eighth 250
         * (218), the dotted eighth 375 (328); r2 rests 1000; > raises the
         * octave for the rest of the channel: c is C5 (72), v8 = 136,
         * c#2 = 73 for 1000 (875), [ e f ]2 plays 76 77 twice, and the
         * tie c4&c8 is one 750 ms note held 437 + 250. */
        CHECK(strcmp(lua_global_string(p->L, "ch1"),
                     "0/60/437/187 500/62/218/187 750/64/328/187 2125/72/437/187 "
                     "2625/73/875/136 3625/76/437/136 4125/77/437/136 4625/76/437/136 "
                     "5125/77/437/136 5625/72/687/136") == 0,
              "MML notes, lengths, dots, rests, octave, volume, repeats and ties");
        if (g_failures) {
            printf("  ch1: %s\n", lua_global_string(p->L, "ch1"));
            printf("  ch2: %s\n", lua_global_string(p->L, "ch2"));
        }
        CHECK(strcmp(lua_global_string(p->L, "ch2"), "2:500:43") == 0,
              "second channel: two kicks at the kick's own note");
        CHECK(strcmp(lua_global_string(p->L, "loop"), "true") == 0, "loop flag kept");
        CHECK(strcmp(lua_global_string(p->L, "playing"), "true") == 0, "the track plays");
        program_terminate(p);
    }
    free(prog);
    free(sdk);
}

static void test_lua_presets(void) {
    mock_set_file("preset.lua", PRESET_LUA);
    CHECK(program_boot("preset.lua", NULL), "preset program boots");
    program_t *p = program_top();
    CHECK(p != NULL, "preset program running");
    if (!p) return;
    char expect_id[16];
    snprintf(expect_id, sizeof(expect_id), "%d", AUDIO_PRESET_BASE + audio_preset_find("laser"));
    CHECK(strcmp(lua_global_string(p->L, "preset_id"), expect_id) == 0, "SoundPreset returns the id");
    CHECK(strcmp(lua_global_string(p->L, "preset_wave"), "square:-300:96") == 0,
          "SoundPreset returns the spec");
    CHECK(strcmp(lua_global_string(p->L, "preset_bad"), "no built-in sound 'nothing'") == 0,
          "unknown preset reported");
    CHECK(strcmp(lua_global_string(p->L, "play_name"), "1") == 0, "SoundPlay by name");
    CHECK(strcmp(lua_global_string(p->L, "play_piano"), "2") == 0, "SoundPlay instrument at a note");
    CHECK(strcmp(lua_global_string(p->L, "play_bad"), "sound not defined") == 0, "unknown name refused");
    CHECK(strcmp(lua_global_string(p->L, "define_base"), "true") == 0, "SoundDefine from a base");
    CHECK(p->audio->instruments[3].slide == -900 && p->audio->instruments[3].cutoff == 80 &&
              p->audio->instruments[3].wave == AUDIO_WAVE_SQUARE,
          "base copied then overridden");
    CHECK(strncmp(lua_global_string(p->L, "presets"), "3", 1) == 0 &&
              strstr(lua_global_string(p->L, "presets"), ":lead") != NULL,
          "SoundPresets lists the bank");
    CHECK(strcmp(lua_global_string(p->L, "music"), "true") == 0, "score with a named sound");
    /* The kick event took the preset's own note, not C4. */
    CHECK(p->audio->events[0].note == 43, "score event defaults to the sound's note");
    program_terminate(p);
}

/* ---- ProTracker modules, streamed from the (mock) card ---- */

#include "mod.h"

extern void mock_set_file_bytes(const char *path, const void *data, size_t len);

/* A cell: sample number, period, effect, parameter. */
static void put_cell(uint8_t *pat, int row, int ch, int sample, int period, int fx, int param) {
    uint8_t *c = pat + (row * 4 + ch) * 4;
    c[0] = (uint8_t)((sample & 0xf0) | ((period >> 8) & 0x0f));
    c[1] = (uint8_t)(period & 0xff);
    c[2] = (uint8_t)(((sample & 0x0f) << 4) | (fx & 0x0f));
    c[3] = (uint8_t)param;
}

/* Build a module: sample 1 a short saw (fully resident), sample 2 a long
 * looped ramp (streamed: longer than the head), two patterns. */
#define TEST_MOD_LONG 60000u
static uint8_t *build_mod(size_t *len_out) {
    uint32_t s1_len = 64, s2_len = TEST_MOD_LONG;
    size_t len = 1084 + 3 * 1024 + s1_len + s2_len;
    uint8_t *m = calloc(1, len);
    memcpy(m, "test module", 11);
    uint8_t *s = m + 20;              /* sample 1 */
    memcpy(s, "saw", 3);
    s[22] = (uint8_t)((s1_len / 2) >> 8); s[23] = (uint8_t)(s1_len / 2);
    s[24] = 0; s[25] = 64; s[26] = 0; s[27] = 0; s[28] = 0; s[29] = 1;
    s = m + 50;                       /* sample 2: loop from 1000, 40000 long */
    memcpy(s, "long", 4);
    s[22] = (uint8_t)((s2_len / 2) >> 8); s[23] = (uint8_t)(s2_len / 2);
    s[24] = 0; s[25] = 48;
    s[26] = (uint8_t)((1000 / 2) >> 8); s[27] = (uint8_t)(1000 / 2);
    s[28] = (uint8_t)((40000 / 2) >> 8); s[29] = (uint8_t)(40000 / 2);
    m[950] = 3;  /* orders: 0, 1, then the long-note pattern */
    m[951] = 0;
    m[952] = 0; m[953] = 1; m[954] = 2;
    memcpy(m + 1080, "M.K.", 4);
    uint8_t *p0 = m + 1084, *p1 = p0 + 1024;
    put_cell(p0, 0, 0, 1, 428, 0xC, 32);   /* C-2 saw, volume 32 */
    put_cell(p0, 0, 1, 2, 214, 0, 0);      /* C-3 long sample */
    put_cell(p0, 0, 3, 0, 0, 0xF, 3);      /* speed 3 */
    put_cell(p0, 1, 0, 0, 0, 0xA, 0x02);   /* volume slide down 2 a tick */
    put_cell(p0, 2, 0, 0, 428, 0x0, 0x47); /* arpeggio +4 +7 */
    put_cell(p0, 3, 0, 0, 0, 0xD, 0x00);   /* pattern break to the next order */
    put_cell(p1, 0, 0, 1, 856, 0, 0);      /* C-1 saw */
    put_cell(p1, 0, 3, 0, 0, 0xF, 200);    /* tempo 200 */
    put_cell(p1, 1, 0, 0, 0, 0xB, 0x02);   /* jump to order 2 */
    uint8_t *p2 = p1 + 1024;
    put_cell(p2, 0, 1, 2, 214, 0, 0);      /* one long note for the streaming test */
    uint8_t *pcm = p2 + 1024;
    for (uint32_t i = 0; i < s1_len; i++) pcm[i] = (uint8_t)(int8_t)((int)(i * 4) - 128);
    for (uint32_t i = 0; i < s2_len; i++) pcm[s1_len + i] = (uint8_t)(int8_t)((i / 100) % 200 - 100);
    *len_out = len;
    return m;
}

static void test_mod(void) {
    size_t len;
    uint8_t *file = build_mod(&len);
    mock_set_file_bytes("song.mod", file, len);
    const char *err = NULL;
    mod_t *m = mod_load("song.mod", &err);
    CHECK(m != NULL, "module loads");
    if (!m) { printf("  %s\n", err ? err : "?"); free(file); return; }
    CHECK(strcmp(m->name, "test module") == 0, "module name");
    CHECK(m->order_count == 3 && m->pattern_count == 3, "orders and patterns");
    CHECK(m->samples[1].length == 64 && m->samples[1].resident_len == 64, "short sample resident");
    CHECK(m->samples[2].length == TEST_MOD_LONG && m->samples[2].resident_len == MOD_HEAD_BYTES &&
              m->samples[2].loop_start == 1000 && m->samples[2].loop_len == 40000,
          "long sample keeps a head and streams");
    CHECK(m->samples[1].resident[1] == (int8_t)(4 - 128), "resident bytes read");
    CHECK(m->pat_num[0] == 0 && m->pat_num[1] == 1, "first two patterns loaded");

    audio_state_t a;
    audio_state_init(&a);
    a.mod = m;
    static int16_t buf[AUDIO_SAMPLE_RATE * 2];
    mod_request_play(m, 1);
    audio_mix(&a, buf, 64); /* tick 0: row 0 */
    CHECK(m->playing && m->row == 0 && m->order_pos == 0, "playing from the top");
    CHECK(m->ch[0].active && m->ch[0].sample == 1 && m->ch[0].volume == 32, "row 0: saw at volume 32");
    CHECK(m->ch[0].step == 5270852u / 428, "C-2 pitch step");
    CHECK(m->ch[1].active && m->ch[1].sample == 2, "row 0: long sample on channel 2");
    CHECK(m->speed == 3, "Fxx set the speed");
    CHECK(buf[10 * 2] != 0 || buf[11 * 2] != 0, "the mix has sound");
    /* Speed 3 at 125 bpm: a row is 3 * 882 frames. Row 1 slides the
     * volume down 2 a tick (ticks 1 and 2): 32 -> 28. */
    audio_mix(&a, buf, 3 * 882);
    CHECK(m->row == 1, "row 1 after three ticks");
    audio_mix(&a, buf, 2 * 882);
    CHECK(m->ch[0].volume == 28, "volume slide");
    audio_mix(&a, buf, 882);
    CHECK(m->row == 2, "row 2");
    audio_mix(&a, buf, 882); /* tick 1 of row 2: arpeggio +4 */
    CHECK(m->ch[0].step == 5270852u / 339, "arpeggio +4 semitones (E-2)");
    audio_mix(&a, buf, 882); /* tick 2: +7 (G-2, period 285); the row ends */
    CHECK(m->ch[0].step == 5270852u / 285, "arpeggio +7 semitones (G-2)");
    CHECK(m->row == 3, "row 3 after the arpeggio row");
    audio_mix(&a, buf, 882 * 3); /* row 3: the break; its last tick jumps */
    CHECK(m->order_pos == 1 && m->row == 0, "pattern break to the next order");
    audio_mix(&a, buf, 882); /* tick 0 of the new pattern's row 0 */
    CHECK(m->bpm == 200 && m->frames_per_tick == AUDIO_SAMPLE_RATE * 5 / 400, "Fxx set the tempo");
    CHECK(m->ch[0].step == 5270852u / 856, "C-1 on the new pattern");
    /* Row 1 of pattern 1 jumps to order 2 (a pattern that was not
     * resident: the player asks for it and waits a tick or two). */
    audio_mix(&a, buf, m->frames_per_tick * 6 + 300);
    CHECK(m->order_pos == 2 && m->row == 0 && m->playing, "position jump to order 2");
    for (int i = 0; i < 4; i++) {
        mod_service(m);
        audio_mix(&a, buf, m->frames_per_tick);
    }
    CHECK(!m->waiting && m->pat_num[m->cur_slot] == 2, "the jump's pattern was loaded on demand");
    CHECK(m->ch[1].sample == 2 && m->ch[1].active && m->ch[1].pos < 1000, "row 0 of it: the long note");

    /* Streaming: channel 2 plays the long sample past its head. Serviced
     * between mixes, the ring stays ahead and nothing underruns; without
     * service it would. */
    uint32_t before = m->underruns;
    for (int i = 0; i < 40; i++) {
        audio_mix(&a, buf, 1024);
        mod_service(m);
    }
    CHECK(m->ch[1].active && m->ch[1].pos > MOD_HEAD_BYTES, "long sample streamed past its head");
    CHECK(m->underruns == before, "no underruns while serviced");
    CHECK(m->ch[1].fill_end > m->ch[1].pos, "ring is ahead of the reader");
    /* The loop wraps: play on until the position passes the loop end. */
    uint32_t frames_to_wrap = (uint32_t)(((uint64_t)41000 << 16) / m->ch[1].step);
    for (uint32_t done = 0; done < frames_to_wrap; done += 1024) {
        audio_mix(&a, buf, 1024);
        mod_service(m);
    }
    CHECK(m->ch[1].active && m->ch[1].pos >= 1000 && m->ch[1].pos < 41000, "sample loop wrapped");

    mod_request_stop(m);
    audio_mix(&a, buf, 64);
    CHECK(!m->playing && !mod_is_playing(m), "stopped");
    a.mod = NULL;
    mod_free(m);
    free(file);

    /* Refusals. */
    uint8_t bad[1084] = {0};
    memcpy(bad + 1080, "8CHN", 4);
    bad[950] = 1;
    mock_set_file_bytes("eight.mod", bad, sizeof(bad));
    CHECK(mod_load("eight.mod", &err) == NULL && strstr(err, "4-channel"), "8-channel refused");
    CHECK(mod_load("missing.mod", &err) == NULL, "missing file refused");
}

static const char *MOD_LUA =
    "results = {}\n"
    "function setup()\n"
    "  results.load = tostring(ModLoad('song.mod'))\n"
    "  local _, err = ModLoad('missing.mod')\n"
    "  results.load_bad = tostring(err)\n"
    "  results.load2 = tostring(ModLoad('song.mod'))\n"
    "  results.play = tostring(ModPlay())\n"
    "  results.playing = tostring(ModPlaying())\n"
    "  local info = ModInfo()\n"
    "  results.info = info.name .. ':' .. info.orders .. ':' .. info.samples\n"
    "end\n"
    "function tick() end\n";

static void test_lua_mod(void) {
    size_t len;
    uint8_t *file = build_mod(&len);
    mock_set_file_bytes("song.mod", file, len);
    mock_set_file("modtest.lua", MOD_LUA);
    CHECK(program_boot("modtest.lua", NULL), "mod program boots");
    program_t *p = program_top();
    if (p) {
        CHECK(strcmp(lua_global_string(p->L, "load"), "true") == 0, "ModLoad");
        CHECK(strcmp(lua_global_string(p->L, "load_bad"), "cannot open module") == 0, "ModLoad error");
        CHECK(strcmp(lua_global_string(p->L, "load2"), "true") == 0, "a second load replaces the first");
        CHECK(strcmp(lua_global_string(p->L, "play"), "true") == 0 &&
                  strcmp(lua_global_string(p->L, "playing"), "true") == 0, "ModPlay");
        CHECK(strcmp(lua_global_string(p->L, "info"), "test module:3:2") == 0, "ModInfo");
        static int16_t buf[2048 * 2];
        audio_mix(p->audio, buf, 2048);
        audio_service();
        CHECK(p->audio->mod && p->audio->mod->playing && p->audio->mod->ch[0].active, "the program's module plays");
        program_terminate(p);
    }
    free(file);
}

static void test_lua_module(void) {
    int16_t ramp[8];
    for (int i = 0; i < 8; i++) {
        ramp[i] = (int16_t)((i + 1) * 1000);
    }
    push_wav(ramp, 8);
    mock_set_file("sound.lua", SOUND_LUA_SRC);
    CHECK(program_boot("sound.lua", NULL), "sound program boots");
    program_t *p = program_top();
    CHECK(p != NULL, "sound program running");
    CHECK(g_current_audio == p->audio, "audio state published");

    CHECK(strcmp(lua_global_string(p->L, "define"), "true") == 0,
          "SoundDefine ok");
    CHECK(strcmp(lua_global_string(p->L, "define_bad"), "sound id out of range (0-31)") == 0,
          "SoundDefine bad id error");
    CHECK(strcmp(lua_global_string(p->L, "wave_bad"), "unknown wave 'bogus'") == 0,
          "SoundDefine bad wave error");
    CHECK(strcmp(lua_global_string(p->L, "music"), "true") == 0,
          "MusicDefine ok");
    CHECK(strcmp(lua_global_string(p->L, "music_play"), "true") == 0,
          "MusicPlay ok");
    CHECK(strcmp(lua_global_string(p->L, "playing"), "true") == 0,
          "MusicPlaying true");
    CHECK(strcmp(lua_global_string(p->L, "play_bad"), "no such score") == 0,
          "MusicPlay missing score error");
    CHECK(strcmp(lua_global_string(p->L, "sound_bad"), "sound not defined") == 0,
          "SoundPlay undefined error");
    CHECK(strcmp(lua_global_string(p->L, "voice"), "1") == 0,
          "SoundPlay returns voice 1");
    CHECK(strcmp(lua_global_string(p->L, "volume"), "true") == 0,
          "SoundVolume ok");
    CHECK(strcmp(lua_global_string(p->L, "wavid"), "32") == 0,
          "SoundLoad returns sample id 32");
    CHECK(strcmp(lua_global_string(p->L, "wavplay"), "2") == 0,
          "sample one-shot plays on voice 2");
    CHECK(p->audio->samples[0].defined && p->audio->samples[0].frames == 8,
          "WAV parsed (8 frames)");
    CHECK(p->audio->master == 200, "master volume applied");

    /* Render: the score (C4+E4) and the sample one-shot are audible. */
    static int16_t buf[256 * 2];
    audio_mix(p->audio, buf, 256);
    int nonzero = 0;
    for (int i = 0; i < 256; i++) {
        if (buf[i * 2] != 0) {
            nonzero++;
        }
    }
    CHECK(nonzero > 100, "score audible");
    CHECK(audio_score_playing(p->audio), "score still playing");

    /* MusicStop via the Lua API, applied on the next block. */
    lua_getglobal(p->L, "MusicStop");
    CHECK(lua_pcall(p->L, 0, 0, 0) == LUA_OK, "MusicStop callable");
    audio_mix(p->audio, buf, 64);
    CHECK(!audio_score_playing(p->audio), "MusicStop stops the score");
}

/* ---------------- test 9: audio state across program stack ---------------- */

static const char *A_LUA =
    "function setup()\n"
    "  SoundDefine(0, {wave='square', attack=0, decay=0, sustain=255, release=10})\n"
    "  MusicDefine('a', {channels={{ {at=0,sound=0,note='C4',dur=500},"
    " {at=100,sound=0,note='E4',dur=500} }}})\n"
    "  MusicPlay('a')\n"
    "end\n"
    "function tick()\n"
    "  if Pid() == 0 then Launch('b.lua') end\n"
    "end\n";

static const char *B_LUA =
    "function tick() ExitProgram() end\n";

static void test_program_stack(void) {
    mock_set_file("a.lua", A_LUA);
    mock_set_file("b.lua", B_LUA);
    CHECK(program_boot("a.lua", NULL), "A boots");
    program_t *a = program_top();
    audio_state_t *aa = a->audio;
    static int16_t buf[64 * 2];
    audio_mix(aa, buf, 64);
    CHECK(buf[20 * 2] != 0, "A score audible");
    CHECK(aa->score_active, "A score active");

    const char *err = NULL;
    CHECK(program_launch("b.lua", NULL, &err) == 0, "B launches");
    program_t *b = program_top();
    CHECK(b->pid == 1 && g_current_audio == b->audio, "B audio published");
    CHECK(!aa->voices[0].active, "A voices silenced while paused");
    CHECK(aa->score_active, "A score position kept");

    program_scheduler_step(); /* B tick -> ExitProgram */
    CHECK(program_top() == a, "A resumed");
    CHECK(g_current_audio == aa, "A audio restored");
    /* Event 2 is due at score frame 4410; the cursor was preserved. */
    static int16_t big[4410 * 2];
    audio_mix(aa, big, 4410);
    CHECK(big[4400 * 2] != 0, "A score resumes from position");
}

/* ---------------- main ---------------- */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); /* keep the last test's name on an abort */
    printf("=== audio tests ===\n");
    rpc_bind_wait(rpc_wait_host);
    rpc_bind_signal(NULL);
    if (!fs_core0_mount()) {
        printf("FAIL: mock SD mount\n");
        return 1;
    }

    test_note_parse();
    test_square();
    test_envelope();
    test_score_schedule();
    test_loop();
    test_pan_volume();
    test_sample();
    test_effects();
    test_presets();
    test_lua_module();
    test_lua_presets();
    test_mml();
    test_mod();
    test_lua_mod();
    test_program_stack();

    if (g_failures == 0) {
        printf("all audio tests passed\n");
        return 0;
    }
    printf("%d audio test(s) FAILED\n", g_failures);
    return 1;
}
