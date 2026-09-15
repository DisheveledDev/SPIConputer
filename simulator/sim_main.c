/* simulator/sim_main.c
 *
 * Desktop simulator for the SPIComputer OS (macOS, SDL2).
 *
 * Runs the real OS C code — Lua programs, process model, RPC, fs bridge,
 * video renderer, audio engine — with the hardware-specific layers
 * replaced by desktop implementations:
 *
 *   FatFs / SD card  ->  a real host folder (default "sdcard")
 *   HSTX / HDMI      ->  an SDL window fed by render_line()
 *   HDMI audio       ->  SDL audio queue fed by audio_mix()
 *   keyboard matrix  ->  SDL keyboard / game controllers -> input events
 *   both cores       ->  one thread: scheduler steps + inline RPC service
 *
 * Usage: spicomputer_sim [--sdcard DIR] [--boot FILE] [--ticks N]
 *                        [--headless] [--exit-after-ms N]
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <stdbool.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lua.h"
#include "lauxlib.h"

#include "audio.h"
#include "fs_core0.h"
#include "input.h"
#include "os_time.h"
#include "program.h"
#include "render.h"
#include "rpc.h"
#include "sim_fs.h"
#include "system_state.h"
#include "video.h"

#define SIM_W 640
#define SIM_H 480

typedef struct {
    const char *sdcard;
    const char *boot_file;
    const char *dump_frame;
    const char *dump_text;
    const char *check_file;
    const char *compile_in;
    const char *compile_out;
    const char *type_text;
    int type_delay_ms;
    int ticks_per_frame;
    int exit_after_ms;
    bool headless;
    bool strip;
} sim_opts_t;

static volatile bool s_running = true;
static bool s_restore_held;
static SDL_GameController *s_pads[2];

/* ------------------------------------------------------------------ */
/* Compile check (used by the IDE)                                     */
/* ------------------------------------------------------------------ */

static int check_lua_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return 2;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);

    lua_State *L = luaL_newstate();
    if (!L) {
        free(buf);
        return 2;
    }
    int rc = 0;
    if (luaL_loadbufferx(L, buf, got, path, "t") != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        fprintf(stderr, "%s\n", message ? message : "compile failed");
        rc = 1;
    } else {
        printf("ok\n");
    }
    lua_close(L);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Bytecode compile (used by the IDE to produce .prg files)            */
/* ------------------------------------------------------------------ */

static int dump_writer(lua_State *L, const void *p, size_t size, void *ud) {
    (void)L;
    FILE *f = (FILE *)ud;
    return fwrite(p, 1, size, f) == size ? 0 : 1;
}

/* Compile a Lua source file into a binary chunk (`luac` format) with
 * the OS's own Lua build, so the result loads on the device and in the
 * simulator. The chunk name is the source's base name, so runtime
 * errors read like on-card ones ("program.lua:12: ..."). `strip` drops
 * the debug info (line numbers, local and upvalue names): the resident
 * shell opts in, trading line numbers in its errors for heap. */
static int compile_lua_file(const char *in_path, const char *out_path, bool strip) {
    FILE *in = fopen(in_path, "rb");
    if (!in) {
        fprintf(stderr, "cannot open %s\n", in_path);
        return 2;
    }
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    char *buf = (char *)malloc(size > 0 ? (size_t)size : 1);
    size_t got = buf ? fread(buf, 1, (size_t)size, in) : 0;
    fclose(in);
    if (!buf || got != (size_t)size) {
        fprintf(stderr, "cannot read %s\n", in_path);
        free(buf);
        return 2;
    }

    const char *base = strrchr(in_path, '/');
    base = base ? base + 1 : in_path;
    char chunk_name[PATH_MAX];
    snprintf(chunk_name, sizeof(chunk_name), "@%s", base);

    lua_State *L = luaL_newstate();
    if (!L) {
        free(buf);
        return 2;
    }
    int rc = 0;
    if (luaL_loadbufferx(L, buf, got, chunk_name, "t") != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        fprintf(stderr, "%s\n", message ? message : "compile failed");
        rc = 1;
    } else {
        FILE *out = fopen(out_path, "wb");
        if (!out) {
            fprintf(stderr, "cannot write %s\n", out_path);
            rc = 2;
        } else {
            if (lua_dump(L, dump_writer, out, strip ? 1 : 0) != 0) {
                fprintf(stderr, "bytecode dump failed\n");
                rc = 1;
            }
            fclose(out);
            if (rc != 0) {
                remove(out_path);
            }
        }
    }
    lua_close(L);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Input: SDL -> input_event_t                                         */
/* ------------------------------------------------------------------ */

static uint8_t sim_mods(void) {
    SDL_Keymod m = SDL_GetModState();
    uint8_t mods = 0;
    if (m & KMOD_SHIFT) {
        mods |= INPUT_MOD_SHIFT;
    }
    if (m & KMOD_CTRL) {
        mods |= INPUT_MOD_CTRL;
    }
    if (m & KMOD_LALT) {
        mods |= INPUT_MOD_CBM; /* left Alt plays the Commodore key */
    }
    if (s_restore_held) {
        mods |= INPUT_MOD_RESTORE;
    }
    return mods;
}

static void push_key(int key, bool down) {
    input_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_EV_KEY;
    ev.key = (uint8_t)key;
    ev.mods = sim_mods();
    ev.pressed = down ? 1 : 0;
    input_queue_push(&g_system_state.input, &ev);
}

/* --type: scripted keystrokes, one per frame, starting half a second
 * after boot so the program has drawn its first screen. Escapes: \n
 * Return, \e Escape, \u \d \l \r cursor keys, \\ backslash. Returns the
 * next key and advances, or 0 at the end of the text. */
static int typed_key(const char **text) {
    const char *p = *text;
    if (!p || !*p) {
        return 0;
    }
    int key = (unsigned char)*p++;
    if (key == '\\' && *p) {
        switch (*p++) {
        case 'n': key = 13; break;
        case 'e': key = 27; break;
        case 'u': key = INPUT_KEY_UP; break;
        case 'd': key = INPUT_KEY_DOWN; break;
        case 'l': key = INPUT_KEY_LEFT; break;
        case 'r': key = INPUT_KEY_RIGHT; break;
        case '1': case '2': case '3': case '4': case '5': case '6': case '7':
            key = INPUT_KEY_F1 + (p[-1] - '1'); break; /* \1..\7 = F1..F7 */
        default: key = '\\'; p--; break;
        }
    }
    *text = p;
    return key;
}

static void push_control(int ctrl, uint8_t dirs, bool down) {
    input_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ctrl == 1 ? INPUT_EV_CONTROL1 : INPUT_EV_CONTROL2;
    ev.ctrl = (uint8_t)ctrl;
    ev.dirs = dirs;
    ev.pressed = down ? 1 : 0;
    input_queue_push(&g_system_state.input, &ev);
}

/* US-layout shifted characters for the number/punctuation rows. */
static int shift_char(int c) {
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 'A';
    }
    const char *unshifted = "1234567890-=[]\\;',./`";
    const char *shifted = "!@#$%^&*()_+{}|:\"<>?~";
    const char *p = strchr(unshifted, c);
    return p ? shifted[p - unshifted] : c;
}

static int extended_key(SDL_Keycode sym) {
    switch (sym) {
    case SDLK_UP:
        return INPUT_KEY_UP;
    case SDLK_DOWN:
        return INPUT_KEY_DOWN;
    case SDLK_LEFT:
        return INPUT_KEY_LEFT;
    case SDLK_RIGHT:
        return INPUT_KEY_RIGHT;
    case SDLK_F1:
        return INPUT_KEY_F1;
    case SDLK_F2:
        return INPUT_KEY_F2;
    case SDLK_F3:
        return INPUT_KEY_F3;
    case SDLK_F4:
        return INPUT_KEY_F4;
    case SDLK_F5:
        return INPUT_KEY_F5;
    case SDLK_F6:
        return INPUT_KEY_F6;
    case SDLK_F7:
        return INPUT_KEY_F7;
    case SDLK_HOME:
        return INPUT_KEY_HOME;
    case SDLK_F9:
        return INPUT_KEY_RUNSTOP;
    default:
        return -1;
    }
}

/* Numpad doubles as joystick 1 (no keyboard collision): 8/2/4/6 + 0. */
static uint8_t numpad_dir(SDL_Keycode sym) {
    switch (sym) {
    case SDLK_KP_8:
        return INPUT_DIR_UP;
    case SDLK_KP_2:
        return INPUT_DIR_DOWN;
    case SDLK_KP_4:
        return INPUT_DIR_LEFT;
    case SDLK_KP_6:
        return INPUT_DIR_RIGHT;
    case SDLK_KP_0:
        return INPUT_DIR_FIRE;
    default:
        return 0;
    }
}

static int key_for(SDL_KeyboardEvent *e) {
    SDL_Keycode sym = e->keysym.sym;
    switch (sym) {
    case SDLK_BACKSPACE:
        return 8;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        return 13;
    case SDLK_TAB:
        return 9;
    case SDLK_ESCAPE:
        return 27;
    case SDLK_DELETE:
        return 127;
    default:
        break;
    }
    if (sym >= 32 && sym < 127) {
        int c = (int)sym;
        if (e->keysym.mod & KMOD_SHIFT) {
            c = shift_char(c);
        }
        return c;
    }
    return extended_key(sym);
}

static void on_key(SDL_KeyboardEvent *e, bool down) {
    /* The hardware matrix emits edges only, so drop macOS key repeats:
     * keep simulation behaviour identical to the board. */
    if (e->repeat) {
        return;
    }
    /* Numpad joystick emulation. */
    uint8_t dirs = numpad_dir(e->keysym.sym);
    if (dirs) {
        push_control(1, dirs, down);
        return;
    }
    /* RESTORE (F8) is a modifier key event with key 0. */
    if (e->keysym.sym == SDLK_F8) {
        if (down && !e->repeat) {
            s_restore_held = true;
        }
        if (!down) {
            s_restore_held = false;
        }
        push_key(0, down);
        return;
    }
    /* Modifier keys report themselves as key 0 with their mod bit. */
    switch (e->keysym.sym) {
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
    case SDLK_LCTRL:
    case SDLK_RCTRL:
    case SDLK_LALT:
        push_key(0, down);
        return;
    default:
        break;
    }
    int key = key_for(e);
    if (key >= 0) {
        push_key(key, down);
    }
}

static void pad_open(int device_index) {
    for (int i = 0; i < 2; i++) {
        if (!s_pads[i]) {
            SDL_GameController *pad = SDL_GameControllerOpen(device_index);
            if (pad) {
                s_pads[i] = pad;
                printf("[sim] game controller %d -> joystick %d: %s\n", i + 1,
                       i + 1, SDL_GameControllerName(pad));
            }
            return;
        }
    }
}

static void pad_button(SDL_ControllerButtonEvent *e) {
    for (int i = 0; i < 2; i++) {
        if (!s_pads[i]) {
            continue;
        }
        SDL_Joystick *js = SDL_GameControllerGetJoystick(s_pads[i]);
        if (SDL_JoystickInstanceID(js) != e->which) {
            continue;
        }
        uint8_t dirs = 0;
        switch (e->button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            dirs = INPUT_DIR_UP;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            dirs = INPUT_DIR_DOWN;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            dirs = INPUT_DIR_LEFT;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            dirs = INPUT_DIR_RIGHT;
            break;
        case SDL_CONTROLLER_BUTTON_A:
        case SDL_CONTROLLER_BUTTON_B:
            dirs = INPUT_DIR_FIRE;
            break;
        default:
            return;
        }
        push_control(i + 1, dirs, e->type == SDL_CONTROLLERBUTTONDOWN);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Video / audio pump                                                  */
/* ------------------------------------------------------------------ */

/* The simulator plays core 0's part: apply the ops the programs queued
 * and render the resulting screen slot. */
static const video_state_t *sim_screen(void) {
    video_ops_drain();
    return video_screen();
}

/* A program that queues more than VIDEO_QUEUE_OPS in one tick blocks in
 * video_op_put until core 0 drains; here that would be a deadlock, so
 * drain from inside the wait, as the board's frame boundary would. */
void video_queue_full_hook(void) {
    video_ops_drain();
}

/* The frame counter core 0 writes at each vertical blank: here it
 * follows wall-clock time at 60 Hz, advanced once per simulator frame
 * and from inside WaitVSync's spin, so a program that waits for frames
 * (a game's tick) runs at frame rate instead of timing out. */
static uint64_t s_frame_clock_start_us;

#define SIM_FRAME_US 16667u

/* The 60 Hz frame the wall clock is in. */
static uint32_t sim_frame_index(void) {
    return (uint32_t)((os_time_us() - s_frame_clock_start_us) / SIM_FRAME_US);
}

static void sim_advance_frames(void) {
    uint32_t frames = sim_frame_index();
    if ((int32_t)(frames - g_system_state.video_frame_count) > 0) {
        g_system_state.video_frame_count = frames;
    }
}

/* WaitVSync spins on this until the frame counter moves. Sleep through
 * most of the wait rather than burning a host core; the main loop sees
 * the frame change afterwards and ends its tick batch there, so the
 * frame the program then draws is rendered straight away. */
void video_frame_wait_hook(void) {
    sim_advance_frames();
    video_ops_drain();
    uint64_t into = (os_time_us() - s_frame_clock_start_us) % SIM_FRAME_US;
    if (SIM_FRAME_US - into > 1500u) {
        usleep(1000);
    }
}

static void sim_render(SDL_Renderer *ren, SDL_Texture *tex, uint8_t *frame) {
    const video_state_t *v = sim_screen();
    for (int y = 0; y < SIM_H; y++) {
        render_line(v, y / 2, frame + (size_t)y * SIM_W * 3);
    }
    SDL_UpdateTexture(tex, NULL, frame, SIM_W * 3);

    int output_w, output_h;
    SDL_GetRendererOutputSize(ren, &output_w, &output_h);
    SDL_Rect destination = {0, 0, output_w, output_h};
    if (output_w * 3 > output_h * 4) {
        destination.w = output_h * 4 / 3;
        destination.x = (output_w - destination.w) / 2;
    } else {
        destination.h = output_w * 3 / 4;
        destination.y = (output_h - destination.h) / 2;
    }
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, &destination);
    SDL_RenderPresent(ren);
}

#define AUDIO_PUMP_FRAMES 512

static void sim_pump_audio(SDL_AudioDeviceID dev) {
    if (!dev) {
        return;
    }
    /* Keep ~20 ms queued; audio_mix runs here on the main thread, the
     * same single-producer shape as core 0 on hardware. */
    Uint32 threshold = (Uint32)AUDIO_SAMPLE_RATE * 4u / 50u;
    if (SDL_GetQueuedAudioSize(dev) > threshold) {
        return;
    }
    static int16_t buf[AUDIO_PUMP_FRAMES * 2];
    audio_state_t *a = g_current_audio;
    if (a) {
        audio_mix(a, buf, AUDIO_PUMP_FRAMES);
    } else {
        memset(buf, 0, sizeof(buf));
    }
    SDL_QueueAudio(dev, buf, sizeof(buf));
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0) {
    printf(
        "usage: %s [options]\n"
        "  --sdcard DIR        virtual SD card folder (default: <simulator dir>/sdcard)\n"
        "  --boot FILE         program to boot (default: core/boot.lua; a .prg is fine)\n"
        "  --ticks N           scheduler ticks per frame (default: 64)\n"
        "  --dump-frame FILE   write the final 640x480 frame as a PPM\n"
        "  --dump-text FILE    write the final text screen (40x30, overlay on top)\n"
        "  --check FILE        compile FILE with the OS Lua and exit\n"
        "  --compile IN OUT    compile Lua source IN to a .prg binary chunk and exit\n"
        "  --strip             with --compile: leave out debug info (line numbers,\n"
        "                      local names): about a fifth less heap once loaded\n"
        "  --headless          no window/audio (smoke tests)\n"
        "  --exit-after-ms N   quit automatically after N ms\n"
        "  --type TEXT         type TEXT one key per frame after boot\n"
        "                      (\\n Return, \\e Escape, \\u \\d \\l \\r cursor keys, \\1..\\7 F1..F7)\n"
        "  --type-delay-ms N   wait N ms after boot before typing (default 500;\n"
        "                      keys typed while boot.lua shows its splash are lost)\n",
        argv0);
}

static void dump_frame_ppm(const char *path, uint8_t *frame) {
    const video_state_t *v = sim_screen();
    for (int y = 0; y < SIM_H; y++) {
        render_line(v, y / 2, frame + (size_t)y * SIM_W * 3);
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[sim] cannot write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", SIM_W, SIM_H);
    fwrite(frame, 1, (size_t)SIM_W * SIM_H * 3, f);
    fclose(f);
    printf("[sim] frame written to %s\n", path);
}

/* --dump-text: the final text screen as 30 lines of 40 characters, the
 * overlay composited over the base, for scripted checks that read the
 * screen instead of its pixels. Codes outside printable ASCII (box
 * drawing, blocks) are written as '#'; after a '|' each line marks its
 * inverted cells with '^', so highlights can be checked too. */
static void dump_text_screen(const char *path) {
    const video_state_t *v = sim_screen();
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[sim] cannot write %s\n", path);
        return;
    }
    int cols = VIDEO_COLS, rows = VIDEO_ROWS;
    for (int y = 0; y < rows; y++) {
        char line[VIDEO_COLS + 1];
        char inv[VIDEO_COLS + 1];
        for (int x = 0; x < cols; x++) {
            int idx = y * cols + x;
            uint8_t ch = v->base_char[idx];
            uint8_t attr = v->base_attr[idx];
            if ((v->overlay_attr[idx] & VIDEO_ATTR_TRANSPARENT) == 0) {
                ch = v->overlay_char[idx];
                attr = v->overlay_attr[idx];
            }
            line[x] = (ch >= 32 && ch < 127) ? (char)ch : '#';
            inv[x] = (attr & 0x80) ? '^' : ' ';
        }
        line[cols] = inv[cols] = '\0';
        int end = cols;
        while (end > 0 && inv[end - 1] == ' ') end--;
        inv[end] = '\0';
        fprintf(f, "%s|%s\n", line, inv);
    }
    fclose(f);
    printf("[sim] text written to %s\n", path);
}

static void rpc_wait_sim(void) {
    (void)fs_core0_service();
}

/* The default card folder sits next to the simulator binary
 * (<exe dir>/sdcard), so it is the same folder however the simulator is
 * launched — from a terminal, Finder or a task runner. Relative
 * "sdcard" is the fallback when the executable path cannot be
 * resolved. */
static const char *default_card_folder(const char *argv0) {
    static char folder[PATH_MAX];
    char exe[PATH_MAX];
    if (!argv0 || strchr(argv0, '/') == NULL || realpath(argv0, exe) == NULL) {
        return "sdcard";
    }
    char *slash = strrchr(exe, '/');
    if (!slash) {
        return "sdcard";
    }
    *slash = '\0';
    snprintf(folder, sizeof(folder), "%s/sdcard", exe);
    return folder;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0); /* keep log lines in order */
    sim_opts_t o = {
        .sdcard = NULL,
        .boot_file = "core/boot.lua",
        .ticks_per_frame = 64,
        .type_delay_ms = 500,
        .exit_after_ms = 0,
        .headless = false,
    };
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sdcard") == 0 && i + 1 < argc) {
            o.sdcard = argv[++i];
        } else if (strcmp(argv[i], "--boot") == 0 && i + 1 < argc) {
            o.boot_file = argv[++i];
        } else if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            o.ticks_per_frame = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dump-frame") == 0 && i + 1 < argc) {
            o.dump_frame = argv[++i];
        } else if (strcmp(argv[i], "--dump-text") == 0 && i + 1 < argc) {
            o.dump_text = argv[++i];
        } else if (strcmp(argv[i], "--check") == 0 && i + 1 < argc) {
            o.check_file = argv[++i];
        } else if (strcmp(argv[i], "--strip") == 0) {
            o.strip = true;
        } else if (strcmp(argv[i], "--compile") == 0 && i + 2 < argc) {
            o.compile_in = argv[++i];
            o.compile_out = argv[++i];
        } else if (strcmp(argv[i], "--exit-after-ms") == 0 && i + 1 < argc) {
            o.exit_after_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
            o.type_text = argv[++i];
        } else if (strcmp(argv[i], "--type-delay-ms") == 0 && i + 1 < argc) {
            o.type_delay_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--headless") == 0) {
            o.headless = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (o.check_file) {
        return check_lua_file(o.check_file);
    }
    if (o.compile_in && o.compile_out) {
        return compile_lua_file(o.compile_in, o.compile_out, o.strip);
    }
    if (!o.sdcard) {
        o.sdcard = default_card_folder(argc > 0 ? argv[0] : NULL);
    }
    if (o.headless && o.exit_after_ms == 0) {
        o.exit_after_ms = 1500;
    }

    printf("[sim] SPIComputer OS simulator\n");
    if (!sim_fs_init(o.sdcard)) {
        fprintf(stderr, "[sim] cannot create SD card folder '%s'\n",
                sim_fs_root_abs());
        return 1;
    }
    printf("[sim] SD card folder: %s\n", sim_fs_root_abs());

    rpc_bind_wait(rpc_wait_sim);
    rpc_bind_signal(NULL);
    if (!fs_core0_mount()) {
        fprintf(stderr, "[sim] cannot mount the virtual SD card\n");
        return 1;
    }
    video_screens_init();
    s_frame_clock_start_us = os_time_us();
    if (!program_boot(o.boot_file, NULL)) {
        fprintf(stderr,
                "[sim] boot failed: no %s in %s (copy your programs in)\n",
                o.boot_file, sim_fs_root_abs());
        return 1;
    }
    /* The heap figure is the Lua allocation right after load + setup,
     * against the per-program cap (inflated on a 64-bit host: pointers
     * and Lua's internal structs are larger than on the device). */
    printf("[sim] booted %s (pid 0), %d ticks/frame, heap %zu/%zu KB\n",
           o.boot_file, o.ticks_per_frame,
           program_top() ? program_top()->heap_used / 1024 : 0,
           program_top() ? program_top()->heap_cap / 1024 : 0);

    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    SDL_AudioDeviceID audio = 0;
    static uint8_t frame[SIM_W * SIM_H * 3];

    if (!o.headless) {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO |
                     SDL_INIT_GAMECONTROLLER) != 0) {
            fprintf(stderr, "[sim] SDL_Init: %s\n", SDL_GetError());
            return 1;
        }
        win = SDL_CreateWindow("SPIComputer OS", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, SIM_W, SIM_H,
                               SDL_WINDOW_RESIZABLE);
        if (!win) {
            fprintf(stderr, "[sim] window: %s\n", SDL_GetError());
            return 1;
        }
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        if (!ren) {
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        }
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24,
                                SDL_TEXTUREACCESS_STREAMING, SIM_W, SIM_H);
        if (!ren || !tex) {
            fprintf(stderr, "[sim] renderer: %s\n", SDL_GetError());
            return 1;
        }

        SDL_AudioSpec want, have;
        memset(&want, 0, sizeof(want));
        want.freq = AUDIO_SAMPLE_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio) {
            SDL_PauseAudioDevice(audio, 0);
        } else {
            fprintf(stderr, "[sim] audio disabled: %s\n", SDL_GetError());
        }
        printf(
            "[sim] controls: type on the keyboard; arrows/Home/F1-F7 feed "
            "the C64 keys,\n"
            "      F8 = RESTORE, F9 = RUN/STOP, left Alt = C=, numpad "
            "8/2/4/6/0 = joystick 1,\n"
            "      game controllers = joystick 1/2 (d-pad + A/B).\n");
    }

    uint64_t started_us = os_time_us();
    uint64_t ticks = 0;
    uint64_t frames = 0; /* display frames: renders and input polls */

    while (s_running) {
        if (!o.headless) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                switch (e.type) {
                case SDL_QUIT:
                    s_running = false;
                    break;
                case SDL_KEYDOWN:
                case SDL_KEYUP:
                    on_key(&e.key, e.type == SDL_KEYDOWN);
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                    pad_open(e.cdevice.which);
                    break;
                case SDL_CONTROLLERDEVICEREMOVED:
                    for (int i = 0; i < 2; i++) {
                        if (s_pads[i] &&
                            SDL_JoystickInstanceID(
                                SDL_GameControllerGetJoystick(s_pads[i])) ==
                                e.cdevice.which) {
                            SDL_GameControllerClose(s_pads[i]);
                            s_pads[i] = NULL;
                        }
                    }
                    break;
                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP:
                    pad_button(&e.cbutton);
                    break;
                default:
                    break;
                }
            }
        }

        /* Scripted input: one key per frame once the program has had
         * half a second to draw its first screen. */
        if (o.type_text &&
            os_time_us() - started_us >= (uint64_t)o.type_delay_ms * 1000u) {
            int key = typed_key(&o.type_text);
            if (key) {
                push_key(key, true);
                push_key(key, false);
            }
        }

        /* One display frame: up to ticks_per_frame scheduler steps, but
         * the frame ends as soon as the 60 Hz clock moves on. A game that
         * paces its tick with WaitVSync crosses the boundary inside its
         * first step, so it gets one step per frame and every frame is
         * rendered and has its input polled; without this cut the batch
         * waited through 64 frames between renders. */
        sim_advance_frames();
        uint32_t frame_index = sim_frame_index();
        for (int i = 0; i < o.ticks_per_frame && s_running; i++) {
            program_scheduler_step();
            /* Core 0's part: collect the display ops each tick queued,
             * or a drawing-heavy program fills the queue and blocks. */
            video_ops_drain();
            ticks++;
            if (sim_frame_index() != frame_index) {
                break;
            }
        }

        /* The device reboots when the last program exits (a game that
         * replaced the shell, or the shell itself); here that is a
         * restart of the boot program. Headless runs keep going too, so
         * a scripted test sees the same sequence as the board. */
        if (program_top() == NULL) {
            printf("[sim] program stack empty: restarting %s\n", o.boot_file);
            video_screens_init();
            if (!program_boot(o.boot_file, NULL)) {
                fprintf(stderr, "[sim] restart failed\n");
                break;
            }
        }

        if (!o.headless) {
            sim_pump_audio(audio);
            sim_render(ren, tex, frame);
        }
        frames++;

        if (o.exit_after_ms > 0 &&
            os_time_us() - started_us >= (uint64_t)o.exit_after_ms * 1000) {
            break;
        }

        if (!o.headless) {
            /* Pace to 60 fps (like a monitor refresh): sleep until the
             * next frame boundary, unless the batch already ran past it. */
            while (sim_frame_index() == frame_index) {
                SDL_Delay(1);
            }
        }
    }

    if (o.dump_frame) {
        dump_frame_ppm(o.dump_frame, frame);
    }
    if (o.dump_text) {
        dump_text_screen(o.dump_text);
    }

    /* Smoke summary (useful with --headless). */
    program_t *p = program_top();
    int painted = 0;
    const video_state_t *screen = sim_screen();
    if (screen) {
        int n = video_mode_cols(screen->mode) *
                video_mode_rows(screen->mode);
        for (int i = 0; i < n; i++) {
            if (screen->base_char[i] != ' ' &&
                screen->base_char[i] != 0) {
                painted++;
            }
        }
    }
    printf("[sim] exit: ticks=%llu, frames=%llu, programs=%d, video=%dx%d@%d "
           "(chars=%d), audio=%s\n",
           (unsigned long long)ticks, (unsigned long long)frames,
           p ? (int)(p->pid + 1) : 0,
           video_mode_cols(screen->mode), video_mode_rows(screen->mode),
           screen->mode, painted,
           audio ? "on" : (o.headless ? "off (headless)" : "unavailable"));

    if (!o.headless) {
        if (audio) {
            SDL_CloseAudioDevice(audio);
        }
        if (tex) SDL_DestroyTexture(tex);
        if (ren) SDL_DestroyRenderer(ren);
        if (win) SDL_DestroyWindow(win);
        SDL_Quit();
    }
    return p ? 0 : 1;
}
