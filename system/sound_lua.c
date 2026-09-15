/* sound_lua.c
 *
 * Sound/music API for programs (Phase 8). Definitions (sounds, scores,
 * loaded samples) compile into the current program's audio state
 * (g_current_audio, outside the Lua heap); playback is a set of
 * requests picked up by the audio producer on its next block (see
 * audio.h). WAV files stream from the SD card via the fs RPC into a
 * per-program sample pool.
 *
 *   SoundDefine(id, spec)          -> true | nil, err
 *   SoundPreset(name)              -> id, spec | nil, err  (the built-in bank)
 *   SoundPresets()                 -> { {name=, effect=, id=}, ... }
 *   SoundLoad(path [, root [, loop_start, loop_end]]) -> sound id | nil, err
 *                                     (WAV PCM; root = the note recorded,
 *                                     default C4; a loop in frames sustains)
 *   ModLoad(path) / ModPlay([loop]) / ModStop() / ModPlaying()
 *   ModPosition() -> order, row, pattern;  ModInfo() -> table;  ModUnload()
 *                                     a ProTracker module streamed from the
 *                                     card (mod.h); one per program
 *   SoundPlay(sound [, note [, dur_ms [, vol [, pan]]]]) -> voice | nil, err
 *   SoundStop([voice])             -> bool (no arg: all one-shots)
 *   SoundStopAll()                 -> true (score + one-shots)
 *   SoundVolume(v)                 -> true
 *   MusicDefine(name, spec)        -> true | nil, err
 *   MusicPlay(name [, loop])       -> true | nil, err
 *   MusicStop()                    -> true
 *   MusicPlaying()                 -> bool
 *
 * Sound spec:  {wave="square"|"pulse"|"triangle"|"saw"|"sine"|"noise",
 *               duty=1..15, attack=ms, decay=ms, sustain=0..255,
 *               release=ms, volume=0..255, note=name|midi,
 *               slide=semitones/s, vibrato=cents, vibrato_rate=Hz,
 *               arp=semitones, arp2=semitones, arp_ms=ms, arp_loop=bool,
 *               cutoff=0..255, base="preset name"}   (all optional;
 *               `base` starts from a built-in sound, the rest override)
 *
 * A `sound` argument (SoundPlay, score events) is an id, or the name of
 * a built-in sound ("laser", "piano": audio_presets.c), whose id is
 * AUDIO_PRESET_BASE + its index. A play without a note uses the sound's
 * own note (effects carry theirs), else C4.
 *
 * Score spec:  {loop=bool, channels={{event, ...}, ...}} with up to 8
 * channels and events {at=ms, sound=id, note=name|midi, dur=ms,
 * vol=0..255, pan=-64..63}. `note` defaults to C4, `vol` to 255,
 * `pan` to 0; `dur` 0/absent means the one-shot envelope. A channel may
 * instead be a packed string of 10-byte records (see music_define).
 */
#include "sound_lua.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"

#include "audio.h"
#include "mod.h"
#include "program.h"
#include "ff.h"
#include "fs_lua.h"

static audio_state_t *current(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "_spi_program");
    program_t *p = (program_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!p || !p->requires_audio || !g_current_audio) {
        luaL_error(L, "sound API called outside a program");
    }
    return g_current_audio;
}

/* ------------------------------------------------------------------ */
/* Table field helpers (the spec table is at the given stack index)    */
/* ------------------------------------------------------------------ */

static lua_Integer field_int(lua_State *L, int idx, const char *k,
                             lua_Integer def) {
    lua_getfield(L, idx, k);
    lua_Integer v = def;
    if (!lua_isnil(L, -1)) {
        v = luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);
    return v;
}

static bool field_bool(lua_State *L, int idx, const char *k, bool def) {
    lua_getfield(L, idx, k);
    bool v = lua_isnil(L, -1) ? def : lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}

/* Note argument: name string ("C4", "A#3") or MIDI number. */
static int note_arg(lua_State *L, int idx, int def) {
    if (lua_isnoneornil(L, idx)) {
        return def;
    }
    if (lua_type(L, idx) == LUA_TSTRING) {
        int n = audio_note_parse(lua_tostring(L, idx));
        if (n < 0) {
            return luaL_error(L, "bad note name: %s", lua_tostring(L, idx));
        }
        return n;
    }
    return (int)luaL_checkinteger(L, idx);
}

static lua_Integer clamp_int(lua_Integer v, lua_Integer lo, lua_Integer hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ------------------------------------------------------------------ */
/* SoundDefine                                                         */
/* ------------------------------------------------------------------ */

static int wave_from_name(const char *name) {
    if (strcmp(name, "square") == 0 || strcmp(name, "pulse") == 0) {
        return AUDIO_WAVE_SQUARE;
    }
    if (strcmp(name, "triangle") == 0 || strcmp(name, "tri") == 0) {
        return AUDIO_WAVE_TRIANGLE;
    }
    if (strcmp(name, "saw") == 0 || strcmp(name, "sawtooth") == 0) {
        return AUDIO_WAVE_SAW;
    }
    if (strcmp(name, "sine") == 0) {
        return AUDIO_WAVE_SINE;
    }
    if (strcmp(name, "noise") == 0) {
        return AUDIO_WAVE_NOISE;
    }
    return -1;
}

static const char *wave_name(int wave) {
    switch (wave) {
    case AUDIO_WAVE_TRIANGLE: return "triangle";
    case AUDIO_WAVE_SAW: return "saw";
    case AUDIO_WAVE_SINE: return "sine";
    case AUDIO_WAVE_NOISE: return "noise";
    default: return "square";
    }
}

/* A sound argument: an id, or a built-in sound's name. -1 when neither. */
static int sound_arg(lua_State *L, int idx) {
    if (lua_type(L, idx) == LUA_TSTRING) {
        int p = audio_preset_find(lua_tostring(L, idx));
        return p < 0 ? -1 : AUDIO_PRESET_BASE + p;
    }
    return (int)luaL_checkinteger(L, idx);
}

/* Read a spec table at `idx` over `ins` (fields present override). */
static int spec_fields(lua_State *L, int idx, audio_instrument_t *ins) {
    lua_getfield(L, idx, "wave");
    if (!lua_isnil(L, -1)) {
        const char *wname = luaL_checkstring(L, -1);
        int wave = wave_from_name(wname);
        if (wave < 0) {
            return luaL_error(L, "unknown wave '%s'", wname);
        }
        ins->wave = (uint8_t)wave;
    }
    lua_pop(L, 1);
    ins->duty = (uint8_t)clamp_int(field_int(L, idx, "duty", ins->duty), 1, 15);
    ins->attack_ms = (uint8_t)clamp_int(field_int(L, idx, "attack", ins->attack_ms), 0, 255);
    ins->decay_ms = (uint8_t)clamp_int(field_int(L, idx, "decay", ins->decay_ms), 0, 255);
    ins->sustain = (uint8_t)clamp_int(field_int(L, idx, "sustain", ins->sustain), 0, 255);
    ins->release_ms = (uint8_t)clamp_int(field_int(L, idx, "release", ins->release_ms), 0, 255);
    ins->volume = (uint8_t)clamp_int(field_int(L, idx, "volume", ins->volume), 0, 255);
    ins->slide = (int16_t)clamp_int(field_int(L, idx, "slide", ins->slide), -3200, 3200);
    ins->vib_depth = (uint8_t)clamp_int(field_int(L, idx, "vibrato", ins->vib_depth), 0, 255);
    lua_getfield(L, idx, "vibrato_rate");
    if (!lua_isnil(L, -1)) {
        ins->vib_rate = (uint8_t)clamp_int((lua_Integer)(luaL_checknumber(L, -1) * 10.0 + 0.5), 0, 255);
    }
    lua_pop(L, 1);
    ins->arp = (int8_t)clamp_int(field_int(L, idx, "arp", ins->arp), -48, 48);
    ins->arp2 = (int8_t)clamp_int(field_int(L, idx, "arp2", ins->arp2), -48, 48);
    ins->arp_ms = (uint8_t)clamp_int(field_int(L, idx, "arp_ms", ins->arp_ms), 0, 255);
    ins->arp_loop = field_bool(L, idx, "arp_loop", ins->arp_loop != 0) ? 1 : 0;
    ins->cutoff = (uint8_t)clamp_int(field_int(L, idx, "cutoff", ins->cutoff), 0, 255);
    lua_getfield(L, idx, "note");
    if (!lua_isnil(L, -1)) {
        ins->note = (uint8_t)note_arg(L, lua_gettop(L), 60);
    }
    lua_pop(L, 1);
    return 0;
}

static int sound_define(lua_State *L) {
    audio_state_t *a = current(L);
    int id = (int)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (id < 0 || id >= AUDIO_INSTRUMENT_MAX) {
        return luaL_error(L, "sound id out of range (0-%d)", AUDIO_INSTRUMENT_MAX - 1);
    }

    audio_instrument_t ins;
    memset(&ins, 0, sizeof(ins));
    ins.defined = 1;
    ins.wave = AUDIO_WAVE_SQUARE;
    ins.duty = AUDIO_DEFAULT_DUTY;
    ins.sustain = 255;
    ins.volume = AUDIO_DEFAULT_VOLUME;
    ins.cutoff = AUDIO_CUTOFF_OPEN;
    /* base = "name": start from a built-in sound and override fields. */
    lua_getfield(L, 2, "base");
    if (!lua_isnil(L, -1)) {
        const char *base = luaL_checkstring(L, -1);
        int p = audio_preset_find(base);
        if (p < 0) {
            return luaL_error(L, "unknown built-in sound '%s'", base);
        }
        ins = audio_preset(p)->ins;
    }
    lua_pop(L, 1);
    spec_fields(L, 2, &ins);
    a->instruments[id] = ins;
    a->version++;
    lua_pushboolean(L, true);
    return 1;
}

/* Push an instrument as a spec table. */
static void push_spec(lua_State *L, const audio_instrument_t *ins) {
    lua_createtable(L, 0, 16);
    lua_pushstring(L, wave_name(ins->wave)); lua_setfield(L, -2, "wave");
    lua_pushinteger(L, ins->duty); lua_setfield(L, -2, "duty");
    lua_pushinteger(L, ins->attack_ms); lua_setfield(L, -2, "attack");
    lua_pushinteger(L, ins->decay_ms); lua_setfield(L, -2, "decay");
    lua_pushinteger(L, ins->sustain); lua_setfield(L, -2, "sustain");
    lua_pushinteger(L, ins->release_ms); lua_setfield(L, -2, "release");
    lua_pushinteger(L, ins->volume); lua_setfield(L, -2, "volume");
    if (ins->note) { lua_pushinteger(L, ins->note); lua_setfield(L, -2, "note"); }
    lua_pushinteger(L, ins->slide); lua_setfield(L, -2, "slide");
    lua_pushinteger(L, ins->vib_depth); lua_setfield(L, -2, "vibrato");
    lua_pushnumber(L, ins->vib_rate / 10.0); lua_setfield(L, -2, "vibrato_rate");
    lua_pushinteger(L, ins->arp); lua_setfield(L, -2, "arp");
    lua_pushinteger(L, ins->arp2); lua_setfield(L, -2, "arp2");
    lua_pushinteger(L, ins->arp_ms); lua_setfield(L, -2, "arp_ms");
    lua_pushboolean(L, ins->arp_loop); lua_setfield(L, -2, "arp_loop");
    lua_pushinteger(L, ins->cutoff ? ins->cutoff : AUDIO_CUTOFF_OPEN); lua_setfield(L, -2, "cutoff");
}

/* SoundPreset(name) -> id, spec: a built-in sound's playable id and its
 * definition as a table (to tweak and SoundDefine under an own id). */
static int sound_preset(lua_State *L) {
    current(L);
    const char *name = luaL_checkstring(L, 1);
    int p = audio_preset_find(name);
    if (p < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "no built-in sound '%s'", name);
        return 2;
    }
    lua_pushinteger(L, AUDIO_PRESET_BASE + p);
    push_spec(L, &audio_preset(p)->ins);
    return 2;
}

/* SoundPresets() -> { {name=, effect=, id=}, ... } in bank order. */
static int sound_presets(lua_State *L) {
    current(L);
    int n = audio_preset_count();
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        const audio_preset_t *p = audio_preset(i);
        lua_createtable(L, 0, 3);
        lua_pushstring(L, p->name); lua_setfield(L, -2, "name");
        lua_pushboolean(L, p->effect); lua_setfield(L, -2, "effect");
        lua_pushinteger(L, AUDIO_PRESET_BASE + i); lua_setfield(L, -2, "id");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Score compilation                                                   */
/* ------------------------------------------------------------------ */

static int find_score(audio_state_t *a, const char *name, int *free_slot) {
    int free_idx = -1;
    for (int i = 0; i < AUDIO_SCORE_MAX; i++) {
        if (a->scores[i].valid && strcmp(a->scores[i].name, name) == 0) {
            return i;
        }
        if (!a->scores[i].valid && free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_slot) {
        *free_slot = free_idx;
    }
    return -1;
}

static int music_define(lua_State *L) {
    audio_state_t *a = current(L);
    const char *name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (name[0] == '\0' || strlen(name) >= AUDIO_SCORE_NAME_MAX) {
        lua_pushnil(L);
        lua_pushfstring(L, "score name must be 1-%d chars",
                        AUDIO_SCORE_NAME_MAX - 1);
        return 2;
    }
    int free_slot = -1;
    int slot = find_score(a, name, &free_slot);
    if (slot < 0) {
        slot = free_slot;
    }
    if (slot < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "too many scores");
        return 2;
    }

    lua_getfield(L, 2, "channels");
    if (!lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_pushliteral(L, "score needs a channels table");
        return 2;
    }
    int ch_idx = lua_gettop(L);
    int nch = (int)lua_rawlen(L, ch_idx);
    if (nch > AUDIO_CHANNEL_MAX) {
        nch = AUDIO_CHANNEL_MAX;
    }

    /* A channel is a table of event tables, or a packed string of
     * 10-byte records (the Sound framework's Music.Track emits those:
     * far less heap than a table per note): at (u32 LE), sound (u8),
     * note (u8), dur (u16 LE), vol (u8), pan (s8). */
    int counts[AUDIO_CHANNEL_MAX] = {0};
    int total = 0;
    for (int c = 0; c < nch; c++) {
        lua_rawgeti(L, ch_idx, c + 1);
        if (lua_type(L, -1) == LUA_TSTRING) {
            size_t n = 0;
            lua_tolstring(L, -1, &n);
            counts[c] = (int)(n / 10);
        } else if (lua_istable(L, -1)) {
            counts[c] = (int)lua_rawlen(L, -1);
        } else {
            lua_pushnil(L);
            lua_pushfstring(L, "channel %d is not a table or a packed string", c + 1);
            return 2;
        }
        lua_pop(L, 1);
        total += counts[c];
    }
    if (a->events_used + total > AUDIO_SCORE_EVENTS_MAX) {
        lua_pushnil(L);
        lua_pushliteral(L, "score event pool full");
        return 2;
    }

    audio_score_t *sc = &a->scores[slot];
    memset(sc, 0, sizeof(sc[0]));
    uint16_t cursor = a->events_used;
    uint32_t length_ms = 0;

    for (int c = 0; c < nch; c++) {
        lua_rawgeti(L, ch_idx, c + 1);
        int ch_tbl = lua_gettop(L);
        bool packed = lua_type(L, ch_tbl) == LUA_TSTRING;
        const uint8_t *rec = packed ? (const uint8_t *)lua_tostring(L, ch_tbl) : NULL;
        sc->ch[c].start = cursor;
        sc->ch[c].count = (uint16_t)counts[c];
        for (int i = 0; i < counts[c]; i++) {
            audio_event_t ev;
            memset(&ev, 0, sizeof(ev));
            int sound_id;
            if (packed) {
                const uint8_t *r = rec + i * 10;
                ev.time_ms = (uint32_t)r[0] | ((uint32_t)r[1] << 8) | ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
                sound_id = r[4];
                ev.note = r[5];
                ev.dur_ms = (uint32_t)r[6] | ((uint32_t)r[7] << 8);
                ev.volume = r[8];
                ev.pan = (int8_t)r[9];
                ev.sound = (uint8_t)sound_id;
            } else {
                lua_rawgeti(L, ch_tbl, i + 1);
                if (!lua_istable(L, -1)) {
                    lua_pushnil(L);
                    lua_pushfstring(L, "channel %d event %d is not a table", c + 1,
                                    i + 1);
                    return 2;
                }
                ev.time_ms = (uint32_t)clamp_int(field_int(L, -1, "at", 0), 0,
                                                0x7fffffff);
                ev.dur_ms = (uint32_t)clamp_int(field_int(L, -1, "dur", 0), 0, 65535);
                lua_getfield(L, -1, "sound");
                sound_id = lua_isnil(L, -1) ? -1 : sound_arg(L, lua_gettop(L));
                lua_pop(L, 1);
                ev.sound = (uint8_t)clamp_int(sound_id, 0, 255);
                if (sound_id < 0) {
                    lua_pushnil(L);
                    lua_pushfstring(L, "channel %d event %d: unknown sound", c + 1, i + 1);
                    return 2;
                }
                ev.volume =
                    (uint8_t)clamp_int(field_int(L, -1, "vol", AUDIO_DEFAULT_VOLUME),
                                       0, 255);
                ev.pan = (int8_t)clamp_int(field_int(L, -1, "pan", 0), -64, 63);
                lua_getfield(L, -1, "note");
                if (lua_isnil(L, -1)) {
                    const audio_instrument_t *ei = audio_instrument(a, sound_id);
                    ev.note = (uint8_t)((ei && ei->note) ? ei->note : 60); /* the sound's own, or C4 */
                } else if (lua_type(L, -1) == LUA_TSTRING) {
                    int n = audio_note_parse(lua_tostring(L, -1));
                    if (n < 0) {
                        return luaL_error(L, "bad note name: %s", lua_tostring(L, -1));
                    }
                    ev.note = (uint8_t)n;
                } else {
                    ev.note = (uint8_t)clamp_int(luaL_checkinteger(L, -1), 0, 127);
                }
                lua_pop(L, 2); /* note value + event table */
            }
            if (!audio_sound_defined(a, ev.sound)) {
                lua_pushnil(L);
                lua_pushfstring(L, "channel %d event %d: sound %d not defined",
                                c + 1, i + 1, (int)ev.sound);
                return 2;
            }
            /* Keep each channel sorted by time (insertion sort). */
            int pos = cursor;
            while (pos > sc->ch[c].start &&
                   a->events[pos - 1].time_ms > ev.time_ms) {
                a->events[pos] = a->events[pos - 1];
                pos--;
            }
            a->events[pos] = ev;
            cursor++;
            uint32_t end = ev.time_ms +
                           (ev.dur_ms ? ev.dur_ms : AUDIO_ONESHOT_TAIL_MS);
            if (end > length_ms) {
                length_ms = end;
            }
        }
        lua_pop(L, 1); /* channel table or string */
    }
    lua_pop(L, 1); /* channels table */

    a->events_used = cursor;
    sc->valid = 1;
    sc->loop = field_bool(L, 2, "loop", false) ? 1 : 0;
    sc->length_ms = length_ms;
    snprintf(sc->name, sizeof(sc->name), "%s", name);
    a->version++;
    lua_pushboolean(L, true);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

static int sound_play(lua_State *L) {
    audio_state_t *a = current(L);
    int sound = sound_arg(L, 1);
    if (!audio_sound_defined(a, sound)) {
        lua_pushnil(L);
        lua_pushliteral(L, "sound not defined");
        return 2;
    }
    const audio_instrument_t *ins = audio_instrument(a, sound);
    int note = note_arg(L, 2, (ins && ins->note) ? ins->note : 60);
    int dur = (int)clamp_int(luaL_optinteger(L, 3, 0), 0, 65535);
    int vol = (int)clamp_int(luaL_optinteger(L, 4, AUDIO_DEFAULT_VOLUME), 0, 255);
    int pan = (int)clamp_int(luaL_optinteger(L, 5, 0), -64, 63);
    int slot = audio_trigger(a, sound, note, vol, pan, dur);
    if (slot < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "no free sound voice");
        return 2;
    }
    lua_pushinteger(L, slot + 1);
    return 1;
}

static int sound_stop(lua_State *L) {
    audio_state_t *a = current(L);
    if (lua_isnoneornil(L, 1)) {
        audio_stop_all_voices(a);
        lua_pushboolean(L, true);
        return 1;
    }
    int slot = (int)luaL_checkinteger(L, 1) - 1;
    if (slot < 0 || slot >= AUDIO_ONESHOT_VOICES) {
        lua_pushboolean(L, false);
        return 1;
    }
    audio_stop_voice(a, slot);
    lua_pushboolean(L, true);
    return 1;
}

static int sound_stop_all(lua_State *L) {
    audio_state_t *a = current(L);
    audio_stop_all_voices(a);
    audio_stop_score(a);
    lua_pushboolean(L, true);
    return 1;
}

static int sound_volume(lua_State *L) {
    audio_state_t *a = current(L);
    int v = (int)clamp_int(luaL_checkinteger(L, 1), 0, 255);
    a->master = (uint8_t)v;
    lua_pushboolean(L, true);
    return 1;
}

static int music_play(lua_State *L) {
    audio_state_t *a = current(L);
    const char *name = luaL_checkstring(L, 1);
    int slot = find_score(a, name, NULL);
    if (slot < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "no such score");
        return 2;
    }
    int loop = -1;
    if (!lua_isnoneornil(L, 2)) {
        loop = lua_toboolean(L, 2) ? 1 : 0;
    }
    audio_play_score(a, slot, loop);
    lua_pushboolean(L, true);
    return 1;
}

static int music_stop(lua_State *L) {
    audio_state_t *a = current(L);
    audio_stop_score(a);
    lua_pushboolean(L, true);
    return 1;
}

static int music_playing(lua_State *L) {
    audio_state_t *a = current(L);
    lua_pushboolean(L, audio_score_playing(a));
    return 1;
}

/* ------------------------------------------------------------------ */
/* WAV loading                                                         */
/* ------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool read_exact(int32_t handle, uint8_t *dst, size_t n) {
    size_t done = 0;
    while (done < n) {
        size_t got = 0;
        if (fs_lua_read_chunk(handle, dst + done, n - done, &got) != FR_OK ||
            got == 0) {
            return false;
        }
        done += got;
    }
    return true;
}

static bool skip_bytes(int32_t handle, uint32_t n) {
    uint8_t buf[128];
    while (n > 0) {
        size_t want = n > sizeof(buf) ? sizeof(buf) : n;
        size_t got = 0;
        if (fs_lua_read_chunk(handle, buf, want, &got) != FR_OK || got == 0) {
            return false;
        }
        n -= (uint32_t)got;
    }
    return true;
}

/* Parse a PCM (8/16-bit, mono/stereo) WAV into the sample pool.
 * On success returns true and *slot_out is set. */
static bool wav_load(lua_State *L, audio_state_t *a, const char *path,
                     int root, uint32_t loop_start, uint32_t loop_end,
                     int *slot_out, const char **err) {
    int slot = -1;
    for (int i = 0; i < AUDIO_SAMPLE_MAX; i++) {
        if (!a->samples[i].defined) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        *err = "too many samples";
        return false;
    }
    if (!a->sample_pool) {
        a->sample_pool = (int16_t *)malloc(AUDIO_SAMPLE_POOL_MAX);
        if (!a->sample_pool) {
            *err = "out of memory";
            return false;
        }
    }

    char resolved[FS_LUA_PATH_MAX];
    path = fs_lua_resolve_path(L, path, resolved);
    int32_t handle = 0;
    FRESULT fr = fs_lua_open_read(path, &handle);
    if (fr != FR_OK) {
        *err = "cannot open file";
        return false;
    }

    uint8_t hdr[12];
    if (!read_exact(handle, hdr, sizeof(hdr)) || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        *err = "not a WAV file";
        goto fail;
    }

    int channels = 0, bits = 0, rate = 0;
    for (;;) {
        uint8_t ck[8];
        if (!read_exact(handle, ck, sizeof(ck))) {
            *err = "no data chunk";
            goto fail;
        }
        uint32_t size = rd32(ck + 4);
        if (memcmp(ck, "fmt ", 4) == 0) {
            if (size < 16) {
                *err = "bad fmt chunk";
                goto fail;
            }
            uint8_t fmt[16];
            if (!read_exact(handle, fmt, sizeof(fmt))) {
                *err = "truncated fmt chunk";
                goto fail;
            }
            uint16_t tag = rd16(fmt);
            channels = rd16(fmt + 2);
            rate = (int)rd32(fmt + 4);
            bits = rd16(fmt + 14);
            if (tag != 1 || (channels != 1 && channels != 2) ||
                (bits != 8 && bits != 16) || rate <= 0 || rate > 48000) {
                *err = "unsupported WAV (PCM 8/16-bit mono/stereo)";
                goto fail;
            }
            if (size > 16 && !skip_bytes(handle, size - 16 + (size & 1))) {
                *err = "truncated fmt chunk";
                goto fail;
            }
        } else if (memcmp(ck, "data", 4) == 0) {
            if (channels == 0) {
                *err = "data chunk before fmt";
                goto fail;
            }
            int bps = bits / 8;
            uint32_t pool_start = a->sample_pool_used;
            uint32_t remaining = size ? size : 0xFFFFFFFFu;
            uint32_t frames = 0;
            uint8_t buf[1024];
            bool ok = true;
            while (remaining > 0) {
                size_t want = remaining > sizeof(buf) ? sizeof(buf) : remaining;
                size_t got = 0;
                if (fs_lua_read_chunk(handle, buf, want, &got) != FR_OK) {
                    *err = "read error";
                    ok = false;
                    break;
                }
                if (got == 0) {
                    break; /* EOF */
                }
                remaining -= (uint32_t)got;
                size_t nsamples = got / (size_t)bps;
                size_t out_bytes = nsamples * sizeof(int16_t);
                if (a->sample_pool_used + out_bytes > AUDIO_SAMPLE_POOL_MAX) {
                    *err = "sample pool full";
                    ok = false;
                    break;
                }
                int16_t *dst = (int16_t *)((uint8_t *)a->sample_pool +
                                           a->sample_pool_used);
                for (size_t i = 0; i < nsamples; i++) {
                    if (bits == 8) {
                        dst[i] = (int16_t)(((int)buf[i] - 128) * 256);
                    } else {
                        dst[i] = (int16_t)rd16(buf + i * 2);
                    }
                }
                a->sample_pool_used += (uint32_t)out_bytes;
                frames += (uint32_t)(nsamples / (size_t)channels);
            }
            if (size & 1) {
                (void)skip_bytes(handle, 1);
            }
            if (!ok || frames == 0) {
                a->sample_pool_used = pool_start;
                if (ok) {
                    *err = "empty data chunk";
                }
                goto fail;
            }
            audio_pcm_t *smp = &a->samples[slot];
            memset(smp, 0, sizeof(*smp));
            smp->defined = 1;
            smp->root = (uint8_t)root;
            if (loop_end > frames) loop_end = frames;
            if (loop_end > loop_start + 1) {
                smp->loop_start = loop_start;
                smp->loop_len = loop_end - loop_start;
            }
            smp->channels = (uint8_t)channels;
            smp->rate = (uint16_t)rate;
            smp->frames = frames;
            smp->offset = pool_start;
            a->version++;
            *slot_out = slot;
            fs_lua_close_handle(handle);
            return true;
        } else {
            /* Unknown chunk: skip it (plus the word-align pad byte). */
            if (!skip_bytes(handle, size + (size & 1))) {
                *err = "truncated chunk";
                goto fail;
            }
        }
    }

fail:
    fs_lua_close_handle(handle);
    return false;
}

/* SoundLoad(path [, root [, loop_start, loop_end]]): `root` is the note
 * the recording is of (default C4); playing another note shifts the
 * pitch by the difference. A loop (frames) sustains a note by repeating
 * that stretch until it is released. */
static int sound_load(lua_State *L) {
    audio_state_t *a = current(L);
    const char *path = luaL_checkstring(L, 1);
    int root = note_arg(L, 2, 60);
    uint32_t loop_start = (uint32_t)clamp_int(luaL_optinteger(L, 3, 0), 0, 0x7fffffff);
    uint32_t loop_end = (uint32_t)clamp_int(luaL_optinteger(L, 4, 0), 0, 0x7fffffff);
    int slot = 0;
    const char *err = NULL;
    if (!wav_load(L, a, path, root, loop_start, loop_end, &slot, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "load failed");
        return 2;
    }
    lua_pushinteger(L, AUDIO_INSTRUMENT_MAX + slot);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Modules (mod.h)                                                     */
/* ------------------------------------------------------------------ */

/* ModLoad(path) -> true | nil, err: loads a ProTracker module (one per
 * program; a second load replaces the first). */
static int mod_load_lua(lua_State *L) {
    audio_state_t *a = current(L);
    char resolved[FS_LUA_PATH_MAX];
    const char *path = fs_lua_resolve_path(L, luaL_checkstring(L, 1), resolved);
    const char *err = NULL;
    mod_t *m = mod_load(path, &err);
    if (!m) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "load failed");
        return 2;
    }
    mod_t *old = a->mod;
    if (old) {
        mod_request_stop(old);
        a->mod = NULL; /* the producer sees NULL before the free */
        old->playing = 0;
    }
    a->mod = m;
    a->version++;
    if (old) mod_free(old);
    lua_pushboolean(L, true);
    return 1;
}

/* ModPlay([loop]) -> true | nil, err: starts the loaded module from the
 * top (and stops a playing score). */
static int mod_play_lua(lua_State *L) {
    audio_state_t *a = current(L);
    if (!a->mod) {
        lua_pushnil(L);
        lua_pushliteral(L, "no module loaded");
        return 2;
    }
    audio_stop_score(a);
    mod_request_play(a->mod, lua_isnoneornil(L, 1) ? 1 : lua_toboolean(L, 1));
    lua_pushboolean(L, true);
    return 1;
}

static int mod_stop_lua(lua_State *L) {
    audio_state_t *a = current(L);
    mod_request_stop(a->mod);
    lua_pushboolean(L, true);
    return 1;
}

static int mod_playing_lua(lua_State *L) {
    audio_state_t *a = current(L);
    lua_pushboolean(L, mod_is_playing(a->mod));
    return 1;
}

/* ModPosition() -> order, row, pattern (or nil): where the player is. */
static int mod_position_lua(lua_State *L) {
    audio_state_t *a = current(L);
    if (!a->mod) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, a->mod->order_pos);
    lua_pushinteger(L, a->mod->row);
    lua_pushinteger(L, a->mod->order[a->mod->order_pos]);
    return 3;
}

/* ModInfo() -> { name, orders, patterns, samples, resident_kb, underruns } */
static int mod_info_lua(lua_State *L) {
    audio_state_t *a = current(L);
    if (!a->mod) {
        lua_pushnil(L);
        return 1;
    }
    mod_t *m = a->mod;
    int samples = 0;
    for (int i = 1; i <= MOD_SAMPLES; i++) {
        if (m->samples[i].length) samples++;
    }
    lua_createtable(L, 0, 6);
    lua_pushstring(L, m->name); lua_setfield(L, -2, "name");
    lua_pushinteger(L, m->order_count); lua_setfield(L, -2, "orders");
    lua_pushinteger(L, m->pattern_count); lua_setfield(L, -2, "patterns");
    lua_pushinteger(L, samples); lua_setfield(L, -2, "samples");
    lua_pushinteger(L, (lua_Integer)(m->pool_used / 1024)); lua_setfield(L, -2, "resident_kb");
    lua_pushinteger(L, (lua_Integer)m->underruns); lua_setfield(L, -2, "underruns");
    return 1;
}

static int mod_unload_lua(lua_State *L) {
    audio_state_t *a = current(L);
    mod_t *m = a->mod;
    if (m) {
        m->playing = 0;
        a->mod = NULL;
        mod_free(m);
    }
    lua_pushboolean(L, true);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const luaL_Reg sound_funcs[] = {
    {"SoundDefine", sound_define},
    {"SoundPreset", sound_preset},
    {"SoundPresets", sound_presets},
    {"SoundLoad", sound_load},
    {"SoundPlay", sound_play},
    {"SoundStop", sound_stop},
    {"SoundStopAll", sound_stop_all},
    {"SoundVolume", sound_volume},
    {"MusicDefine", music_define},
    {"MusicPlay", music_play},
    {"MusicStop", music_stop},
    {"MusicPlaying", music_playing},
    {"ModLoad", mod_load_lua},
    {"ModPlay", mod_play_lua},
    {"ModStop", mod_stop_lua},
    {"ModPlaying", mod_playing_lua},
    {"ModPosition", mod_position_lua},
    {"ModInfo", mod_info_lua},
    {"ModUnload", mod_unload_lua},
    {NULL, NULL},
};

void sound_lua_openlibs(lua_State *L) {
    lua_pushglobaltable(L);
    luaL_setfuncs(L, sound_funcs, 0);
    lua_pop(L, 1);
}
