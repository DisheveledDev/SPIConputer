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
 * Usage: spicomputer_sim [--sdcard DIR] [--seed-dir DIR] [--ticks N]
 *                        [--headless] [--exit-after-ms N]
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    const char *seed_dir;
    const char *dump_frame;
    const char *check_file;
    int ticks_per_frame;
    int exit_after_ms;
    bool headless;
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

static void sim_render(SDL_Renderer *ren, SDL_Texture *tex, uint8_t *frame) {
    const video_state_t *v = g_current_video;
    if (v) {
        for (int y = 0; y < SIM_H; y++) {
            render_line(v, y, frame + (size_t)y * SIM_W * 3);
        }
    } else {
        memset(frame, 0, SIM_W * SIM_H * 3);
    }
    SDL_UpdateTexture(tex, NULL, frame, SIM_W * 3);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
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
        "  --sdcard DIR        virtual SD card folder (default: ./sdcard)\n"
        "  --seed-dir DIR      copy os.lua/editor.lua from DIR when missing\n"
        "  --ticks N           scheduler ticks per frame (default: 64)\n"
        "  --dump-frame FILE   write the final 640x480 frame as a PPM\n"
        "  --check FILE        compile FILE with the OS Lua and exit\n"
        "  --headless          no window/audio (smoke tests)\n"
        "  --exit-after-ms N   quit automatically after N ms\n",
        argv0);
}

static void dump_frame_ppm(const char *path, uint8_t *frame) {
    const video_state_t *v = g_current_video;
    if (v) {
        for (int y = 0; y < SIM_H; y++) {
            render_line(v, y, frame + (size_t)y * SIM_W * 3);
        }
    } else {
        memset(frame, 0, SIM_W * SIM_H * 3);
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

static void rpc_wait_sim(void) {
    (void)fs_core0_service();
}

int main(int argc, char **argv) {
    sim_opts_t o = {
        .sdcard = "sdcard",
        .seed_dir = SIM_OS_DIR,
        .ticks_per_frame = 64,
        .exit_after_ms = 0,
        .headless = false,
    };
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sdcard") == 0 && i + 1 < argc) {
            o.sdcard = argv[++i];
        } else if (strcmp(argv[i], "--seed-dir") == 0 && i + 1 < argc) {
            o.seed_dir = argv[++i];
        } else if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            o.ticks_per_frame = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dump-frame") == 0 && i + 1 < argc) {
            o.dump_frame = argv[++i];
        } else if (strcmp(argv[i], "--check") == 0 && i + 1 < argc) {
            o.check_file = argv[++i];
        } else if (strcmp(argv[i], "--exit-after-ms") == 0 && i + 1 < argc) {
            o.exit_after_ms = atoi(argv[++i]);
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
    if (o.headless && o.exit_after_ms == 0) {
        o.exit_after_ms = 1500;
    }

    printf("[sim] SPIComputer OS simulator\n");
    if (!sim_fs_init(o.sdcard, o.seed_dir)) {
        fprintf(stderr, "[sim] cannot create SD card folder '%s'\n", o.sdcard);
        return 1;
    }
    printf("[sim] SD card folder: %s\n", sim_fs_root());

    rpc_bind_wait(rpc_wait_sim);
    rpc_bind_signal(NULL);
    if (!fs_core0_mount()) {
        fprintf(stderr, "[sim] cannot mount the virtual SD card\n");
        return 1;
    }
    if (!program_boot("os.lua", NULL)) {
        fprintf(stderr, "[sim] boot failed: no os.lua on the card\n");
        return 1;
    }
    printf("[sim] booted os.lua (pid 0), %d ticks/frame\n", o.ticks_per_frame);

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
                               SDL_WINDOWPOS_CENTERED, SIM_W, SIM_H, 0);
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
    uint64_t frame_start = started_us;
    Uint64 perf_freq = SDL_GetPerformanceFrequency();
    uint64_t ticks = 0;

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

        for (int i = 0; i < o.ticks_per_frame && s_running; i++) {
            atomic_fetch_add_explicit(&g_system_state.heartbeat, 1,
                                      memory_order_relaxed);
            program_scheduler_step();
            ticks++;
        }

        if (!o.headless) {
            sim_pump_audio(audio);
            sim_render(ren, tex, frame);
        }

        if (o.exit_after_ms > 0 &&
            os_time_us() - started_us >= (uint64_t)o.exit_after_ms * 1000) {
            break;
        }

        if (!o.headless) {
            /* Pace to 60 fps (like a monitor refresh). */
            Uint64 budget = perf_freq / 60;
            for (;;) {
                Uint64 now = SDL_GetPerformanceCounter();
                if (now - frame_start >= budget) {
                    break;
                }
                SDL_Delay(1);
            }
            frame_start = SDL_GetPerformanceCounter();
        }
    }

    if (o.dump_frame) {
        dump_frame_ppm(o.dump_frame, frame);
    }

    /* Smoke summary (useful with --headless). */
    program_t *p = program_top();
    int painted = 0;
    if (g_current_video) {
        int n = video_mode_cols(g_current_video->mode) *
                video_mode_rows(g_current_video->mode);
        if (g_current_video->mode == VIDEO_MODE_TEXT40 ||
            g_current_video->mode == VIDEO_MODE_TEXT40C) {
            n = 40 * 30;
        }
        for (int i = 0; i < n; i++) {
            if (g_current_video->char_map[i] != ' ' &&
                g_current_video->char_map[i] != 0) {
                painted++;
            }
        }
    }
    printf("[sim] exit: ticks=%llu, programs=%d, video=%dx%d@%d (chars=%d), "
           "audio=%s\n",
           (unsigned long long)ticks, p ? (int)(p->pid + 1) : 0,
           g_current_video ? video_mode_cols(g_current_video->mode) : 0,
           g_current_video ? video_mode_rows(g_current_video->mode) : 0,
           g_current_video ? g_current_video->mode : -1, painted,
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
