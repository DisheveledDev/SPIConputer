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
    static char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", s ? s : "<nil>");
    lua_pop(L, 2);
    return tmp;
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
    test_lua_module();
    test_program_stack();

    if (g_failures == 0) {
        printf("all audio tests passed\n");
        return 0;
    }
    printf("%d audio test(s) FAILED\n", g_failures);
    return 1;
}
