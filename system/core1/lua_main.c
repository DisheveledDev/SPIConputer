/* core1/lua_main.c
 *
 * Core 1: the OS core. Everything except the video scanout runs here:
 * stdio, the SD card and FatFs (so filesystem calls are direct, with no
 * RPC or locks against the Lua thread), input scanning (1 kHz timer IRQ),
 * the watchdog and the Lua scheduler.
 *
 * Core 0 runs the HSTX scanout only. The single object shared between the
 * cores is g_system_state.video_frame_count, which core 0 writes at each
 * vertical blank; this core reads it for WaitVSync, for the video-state
 * retire queue and for the watchdog gate below.
 *
 * Core affinity rules (see also core0/main.c):
 *   - stdio is initialised here and must only be used from this core;
 *   - the alarm pool (timers, sleep_*) is claimed by whichever core first
 *     uses it, so this core takes it for the 1 kHz input tick and core 0
 *     avoids sleep_ms()/timers entirely;
 *   - each IRQ belongs to the core that enables it: the SD SPI's
 *     DMA_IRQ_0 and the timer IRQ are enabled here, the video IRQs on
 *     core 0.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#if defined(PICO_RP2350)
#include "hardware/structs/powman.h"
#endif

#include "board_config.h"
#include "boot_signal.h"
#include "fs_core0.h"
#include "input_hw.h"
#include "program.h"
#include "rpc.h"
#include "scanout.h"
#include "system_state.h"
#include "video_hw.h"

// Boot program loaded from the SD card root at startup (starts the
// shell, os.lua, in the foreground).
#define BOOT_SCRIPT "core/boot.lua"

/* A frozen display is as fatal as a frozen VM: if the frame counter has
 * not moved for this long, stop feeding the watchdog. */
#define VIDEO_STALL_LIMIT_US 250000

/* Bring-up status period (SPICOMPUTER_LOG_PERIOD_MS, default 1000): a
 * video status line per period goes to USB stdio and to SPILOG_FILE on
 * the card (see sd_log below). */
#ifndef SPICOMPUTER_LOG_PERIOD_MS
#define SPICOMPUTER_LOG_PERIOD_MS 1000
#endif
#if SPICOMPUTER_LOG_PERIOD_MS < 1
#undef SPICOMPUTER_LOG_PERIOD_MS
#define SPICOMPUTER_LOG_PERIOD_MS 1
#endif
#define LOG_PERIOD_US ((uint64_t)SPICOMPUTER_LOG_PERIOD_MS * 1000u)

/* ... but give the display time to come up before enforcing that. */
#define VIDEO_BOOT_GRACE_US 5000000

/* Filesystem calls run inline on this core, so a long `fs.readall` /
 * `fs.writeall` (many 4 KB operations inside one Lua call) can keep the
 * scheduler busy for longer than the watchdog period. A completed
 * filesystem operation is evidence of progress, so refresh the watchdog
 * around each one: a hung SD transfer still stops the feeds (the update
 * below happens before and after, never during), while a program that
 * streams files legitimately no longer trips the reset. */
static void fs_service(const rpc_request_t *req, rpc_response_t *resp) {
    watchdog_update();
    fs_core0_execute(req, resp);
    watchdog_update();
}

/* ------------------------------------------------------------------ */
/* Bring-up log on the SD card                                        */
/*                                                                    */
/* The bench has no working USB console, so the boot report and the  */
/* per-period video status also go to SPILOG_FILE in the card root.   */
/* Lines collect in RAM (lines logged before the mount wait there)    */
/* and sd_log_flush() appends them with one open/append/close, so     */
/* everything flushed is on the card before a reset can lose it.      */
/* Each boot starts with a "=== boot" line.                           */
/* ------------------------------------------------------------------ */

#define SPILOG_FILE "spilog.txt"

static char s_log_buf[2048];
static size_t s_log_len;

static void sd_log(const char *format, ...) {
    char line[200];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line) - 1, format, args);
    va_end(args);
    if (n < 0) {
        return;
    }
    if ((size_t)n > sizeof(line) - 2) {
        n = (int)(sizeof(line) - 2); /* truncated */
    }
    line[n++] = '\n';
    line[n] = '\0';
    fputs(line, stdout);
    if (s_log_len + (size_t)n <= sizeof(s_log_buf)) {
        memcpy(s_log_buf + s_log_len, line, (size_t)n);
        s_log_len += (size_t)n;
    }
}

static void sd_log_flush(void) {
    if (s_log_len == 0 || !fs_core0_mounted()) {
        return; /* no card yet: keep the lines for the next flush */
    }
    fs_core0_append(SPILOG_FILE, s_log_buf, s_log_len);
    s_log_len = 0; /* a failed write is dropped rather than retried */
}

/* Breadcrumbs in the watchdog scratch registers, which survive a
 * watchdog reset (a power-on clears them). Updated as the OS core runs,
 * read back and logged at the next boot, so a reset leaves a record of
 * how long the previous run lasted and what this core was doing when it
 * stopped. Scratch 4-7 belong to the SDK/bootrom. */
#define CRUMB_MAGIC 0x5b10c0deu
#define CRUMB_SCRATCH_MAGIC 0
#define CRUMB_SCRATCH_UPTIME 1
#define CRUMB_SCRATCH_PHASE 2

enum {
    PHASE_BOOT = 1,      /* stdio, card mount, first program load */
    PHASE_SCHEDULER = 2, /* inside program_scheduler_step (Lua) */
    PHASE_LOOP = 3,      /* the loop's own bookkeeping */
    PHASE_LOG = 4,       /* writing spilog.txt */
};

static void crumb_phase(uint32_t phase) {
    watchdog_hw->scratch[CRUMB_SCRATCH_PHASE] = phase;
}

static const char *phase_name(uint32_t phase) {
    switch (phase) {
        case PHASE_BOOT: return "boot";
        case PHASE_SCHEDULER: return "Lua scheduler step";
        case PHASE_LOOP: return "scheduler loop";
        case PHASE_LOG: return "writing the SD log";
        default: return "unknown";
    }
}

static void log_reset_reason(bool was_watchdog) {
    sd_log("=== boot (SPIComputerOS, %s %s)", __DATE__, __TIME__);
#if defined(PICO_RP2350)
    uint32_t r = powman_hw->chip_reset;
    sd_log("reset: chip_reset=0x%08lx%s%s%s%s%s%s%s%s", (unsigned long)r,
           (r & POWMAN_CHIP_RESET_HAD_POR_BITS) ? " power-on" : "",
           (r & POWMAN_CHIP_RESET_HAD_BOR_BITS) ? " BROWN-OUT" : "",
           (r & POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS) ? " RUN-pin" : "",
           (r & POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS) ? " GLITCH" : "",
           (r & (POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_PSM_BITS |
                 POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS |
                 POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_BITS |
                 POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_ASYNC_BITS))
               ? " watchdog"
               : "",
           (r & POWMAN_CHIP_RESET_HAD_RESCUE_BITS) ? " rescue" : "",
           (r & POWMAN_CHIP_RESET_HAD_DP_RESET_REQ_BITS) ? " debugger" : "",
           (r & POWMAN_CHIP_RESET_HAD_HZD_SYS_RESET_REQ_BITS) ? " sys-reset-req"
                                                               : "");
#endif
    if (was_watchdog) {
        sd_log("reset: rebooted by the watchdog timer");
    }
    if (watchdog_hw->scratch[CRUMB_SCRATCH_MAGIC] == CRUMB_MAGIC) {
        uint32_t phase = watchdog_hw->scratch[CRUMB_SCRATCH_PHASE];
        sd_log("reset: previous run lasted %lus; it stopped during: %s",
               (unsigned long)watchdog_hw->scratch[CRUMB_SCRATCH_UPTIME],
               phase_name(phase));
    } else {
        sd_log("reset: no record of a previous run (power-on or RUN pin)");
    }
    watchdog_hw->scratch[CRUMB_SCRATCH_MAGIC] = CRUMB_MAGIC;
    watchdog_hw->scratch[CRUMB_SCRATCH_UPTIME] = 0;
    crumb_phase(PHASE_BOOT);
}

static void print_boot_info(void) {
    uint32_t sys = clock_get_hz(clk_sys);
    sd_log("clocks: sys %lu Hz, peri %lu Hz, usb %lu Hz, refresh %u Hz",
           (unsigned long)sys, (unsigned long)clock_get_hz(clk_peri),
           (unsigned long)clock_get_hz(clk_usb),
           (unsigned)SPICOMPUTER_REFRESH_HZ);
#if defined(PICO_RP2350)
    if (sys / 1000 != SPICOMPUTER_SYS_CLOCK_KHZ) {
        sd_log("overclock: %u kHz is not exactly achievable; using %u kHz",
               (unsigned)SPICOMPUTER_SYS_CLOCK_KHZ, (unsigned)(sys / 1000));
    }
#endif

#if defined(SPICOMPUTER_HAS_HDMI)
    uint32_t div, hstx, pixel;
    bool warn;
    video_hw_clock_info(&div, &hstx, &pixel, &warn);
    sd_log("HSTX: clk_sys %u / %u = %u Hz (pixel clock %u Hz)",
           (unsigned)sys, (unsigned)div, (unsigned)hstx, (unsigned)pixel);
    if (warn) {
        sd_log("HSTX: warning: clk_sys does not divide to a 25.2 MHz "
               "pixel clock; expect ~%u Hz refresh",
               (unsigned)(pixel /
                          (SCANOUT_H_TOTAL_PIXELS * SCANOUT_V_TOTAL_LINES)));
    }
#endif
}

void core1_entry(void)
{
    /* stdio first, so the boot log below is visible. The clock was set by
     * core 0 before this core was launched, so baud rates are correct. */
    stdio_init_all();
    boot_signal(5); /* stdio up: USB should be enumerating */

    /* Arm the watchdog before anything that could hang (the boot prints,
     * the SD mount, the first program load). Nothing feeds it until the
     * scheduler loop below, so a hang anywhere in boot resets the board
     * and it retries - a bad boot can never leave the board silent and
     * unreachable. The period is generous here because a slow card can
     * take seconds to answer. */
    bool was_watchdog = watchdog_caused_reboot();
    watchdog_enable(8000, 1);
    watchdog_update();

    /* Buffered until the card is mounted, then flushed below. */
    log_reset_reason(was_watchdog);
    print_boot_info();

    /* Filesystem: the SD card is on this core now, so the Lua bindings
     * call straight into the FatFs layer with no cross-core round trip.
     * The video core claimed its DMA channels and DMA_IRQ_2 before this
     * core started, so the SD driver gets its own. */
    rpc_set_local_handler(fs_service);
    bool card_ok = false;
#if defined(SPICOMPUTER_VIDEO_TEST_PATTERN) || defined(SPICOMPUTER_CHEQUERBOARD)
    /* Test pattern builds deliberately perform no SD work: this isolates
     * the video core from card-init traffic, DMA and XIP activity. */
    printf("test pattern build: SD mount skipped (no SD log)\n");
#else
    card_ok = fs_core0_mount();
    if (card_ok) {
        sd_log("boot: SD card mounted");
        sd_log_flush();
    } else {
        printf("SD card not mounted (no SD log)\n");
    }
#endif
    /* The mount is the slow, unbounded part of boot (an empty socket can
     * cost a full command timeout); give the program load its own full
     * watchdog period rather than whatever the card left of this one. */
    watchdog_update();
    boot_signal(6); /* card stage complete */

    /* Input subsystem: 1 kHz matrix and joystick scanning. This claims
     * the alarm pool (and its IRQ) for this core. */
    input_hw_init();

    bool boot_failed = false;
#if defined(SPICOMPUTER_VIDEO_TEST_PATTERN)
    /* Diagnostic build: the scanout draws a test pattern, so no program
     * is loaded (the SD mount above still reports the card). */
    printf("test pattern build: boot program not loaded\n");
#elif defined(SPICOMPUTER_CHEQUERBOARD)
    /* Forced chequerboard: the card is never read and no Lua runs, so
     * whatever the monitor shows is the HDMI path alone. */
    video_hw_set_test_pattern(true);
    printf("chequerboard build: boot program not loaded\n");
#else
    /* No card, or a card without a boot program: show the chequerboard
     * instead of a black screen, so a missing card and a dead display
     * look different on the bench. */
    if (!card_ok) {
        boot_failed = true;
        video_hw_set_test_pattern(true);
        printf("no SD card: showing the chequerboard test pattern\n");
    } else if (!program_boot(BOOT_SCRIPT, NULL)) {
        boot_failed = true;
        video_hw_set_test_pattern(true);
        sd_log("boot: %s failed (details in logs/errors); showing the "
               "chequerboard", BOOT_SCRIPT);
    } else {
        sd_log("boot: %s running", BOOT_SCRIPT);
    }
    sd_log_flush();
#endif
    boot_signal(7); /* boot program attempted; scheduler loop next */

    /* Boot is over: from here the loop below is the only thing feeding
     * the watchdog, so a stuck VM, a deadlock or a frozen display resets
     * the system. */
    watchdog_enable(2000, 1);
    watchdog_update();

    uint32_t last_frames = g_system_state.video_frame_count;
    uint64_t last_frame_us = time_us_64();
    uint64_t boot_us = last_frame_us;
    uint64_t stats_us = last_frame_us;
    uint32_t last_underruns = 0;
    uint32_t last_long_gaps = 0;
    bool stall_logged = false;

    for (;;) {
        /* Drain input, run due timers, otherwise tick. */
        crumb_phase(PHASE_SCHEDULER);
        program_scheduler_step();
        crumb_phase(PHASE_LOOP);

        uint64_t now = time_us_64();
        uint32_t frames = g_system_state.video_frame_count;
        if (frames != last_frames) {
            last_frames = frames;
            last_frame_us = now;
        }

#if defined(SPICOMPUTER_HAS_HDMI)
        if (boot_failed) {
            watchdog_update();
        }
        /* Two liveness gates: this loop is running (a stuck VM or a
         * deadlock stops the scheduler step above) and the display is
         * producing frames. On boards without HSTX only the first
         * applies; the frame counter never moves there. */
        bool video_alive = (now - boot_us < VIDEO_BOOT_GRACE_US) ||
                           (now - last_frame_us < VIDEO_STALL_LIMIT_US);
        if (video_alive && !boot_failed) {
            watchdog_update();
        }
        if (!video_alive && !stall_logged) {
            /* The watchdog resets the board in 2 s: get this onto the
             * card first. */
            stall_logged = true;
            sd_log("video: FRAME COUNTER STALLED at frame %lu (scanline "
                   "%lu); the watchdog will reset the board",
                   (unsigned long)frames, (unsigned long)video_hw_scanline());
            crumb_phase(PHASE_LOG);
            sd_log_flush();
            crumb_phase(PHASE_LOOP);
        }
#else
        (void)boot_us;
        (void)last_frame_us;
        watchdog_update();
#endif

        if (now - stats_us >= LOG_PERIOD_US) {
            stats_us = now;
            boot_signal_tick(); /* alive: status-LED blink */
            uint32_t up_s = (uint32_t)(now / 1000000u); /* since reset */
            watchdog_hw->scratch[CRUMB_SCRATCH_UPTIME] = up_s;
#if defined(SPICOMPUTER_HAS_HDMI)
            /* One line per period. Every counter is cumulative, so a
             * dropout that leaves them all flat (with `up` still
             * counting) was not caused by the scanout. */
            uint32_t underruns = video_hw_underruns();
            sd_log("t=%lus frame=%lu underruns=%lu(+%lu) skew=%lu(%lu) "
                   "gap_max=%luus long=%lu empty=%lu wof=%lu late=%lu "
                   "fifo_min=%lu",
                   (unsigned long)up_s, (unsigned long)frames,
                   (unsigned long)underruns,
                   (unsigned long)(underruns - last_underruns),
                   (unsigned long)video_hw_skews(),
                   (unsigned long)video_hw_last_frame_steps(),
                   (unsigned long)video_hw_gap_max_us(),
                   (unsigned long)video_hw_long_gaps(),
                   (unsigned long)video_hw_fifo_empty(),
                   (unsigned long)video_hw_fifo_wofs(),
                   (unsigned long)video_hw_late_posts(),
                   (unsigned long)video_hw_fifo_min());
            uint32_t long_gaps = video_hw_long_gaps();
            if (long_gaps != last_long_gaps) {
                sd_log("  last long gap: %luus ending at frame %lu line %lu, "
                       "fifo %lu, cmdlist next %lu, underruns %lu",
                       (unsigned long)video_hw_last_gap_us(),
                       (unsigned long)video_hw_last_gap_frame(),
                       (unsigned long)video_hw_last_gap_line(),
                       (unsigned long)video_hw_last_gap_fifo_level(),
                       (unsigned long)video_hw_last_gap_cmdlist(),
                       (unsigned long)video_hw_last_gap_underruns());
                last_long_gaps = long_gaps;
            }
            last_underruns = underruns;
#else
            sd_log("t=%lus", (unsigned long)up_s);
#endif
            crumb_phase(PHASE_LOG);
            sd_log_flush();
            crumb_phase(PHASE_LOOP);
        }
    }
}
