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

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"

#include "board_config.h"
#include "boot_signal.h"
#include "fs_core0.h"
#include "input_hw.h"
#include "program.h"
#include "rpc.h"
#include "system_state.h"
#include "video_hw.h"

// Boot program loaded from the SD card root at startup (starts the
// shell, os.lua, in the foreground).
#define BOOT_SCRIPT "core/boot.lua"

/* A frozen display is as fatal as a frozen VM: if the frame counter has
 * not moved for this long, stop feeding the watchdog. */
#define VIDEO_STALL_LIMIT_US 250000

/* Bring-up logging period (SPICOMPUTER_LOG_PERIOD_MS, default 1000).
 * The status line goes to USB stdio and to spilog.txt on the card; a
 * longer period is used to test whether the SD card / stdio writes
 * themselves disturb the display (the dropouts should then space out
 * with it), and 0 disables logging. */
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

/* Once-per-second status line, written to spilog.txt on the mounted card
 * so bring-up runs can be inspected without a serial console (the USB
 * CDC port is not always reachable behind hubs). The counters come from
 * video_hw.c; they read zero on builds without HDMI. */
static void log_status(uint64_t now_us, uint32_t frames) {
    static uint32_t last_long_gaps;
    char line[224];

    snprintf(line, sizeof(line),
             "t=%lus frame=%lu underruns=%lu black=%lu snapfail=%lu "
             "skew=%lu(%lu) gap=%luus long=%lu empty=%lu wof=%lu",
             (unsigned long)(now_us / 1000000u), (unsigned long)frames,
             (unsigned long)video_hw_underruns(),
             (unsigned long)video_hw_black_rows(),
             (unsigned long)video_hw_snapshot_fails(),
             (unsigned long)video_hw_skews(),
             (unsigned long)video_hw_last_frame_steps(),
             (unsigned long)video_hw_gap_max_us(),
             (unsigned long)video_hw_long_gaps(),
             (unsigned long)video_hw_fifo_empty(),
             (unsigned long)video_hw_fifo_wofs());
    fs_core0_log(line);

    /* New stream stalls: log where in the frame the most recent one
     * ended, and what the FIFO looked like when it did. */
    uint32_t gaps = video_hw_long_gaps();
    if (gaps != last_long_gaps) {
        snprintf(line, sizeof(line),
                 "stall: n=%lu gap=%luus frame=%lu line=%lu fifo=%lu "
                 "cmdlist=%lu underruns=%lu",
                 (unsigned long)gaps, (unsigned long)video_hw_last_gap_us(),
                 (unsigned long)video_hw_last_gap_frame(),
                 (unsigned long)video_hw_last_gap_line(),
                 (unsigned long)video_hw_last_gap_fifo_level(),
                 (unsigned long)video_hw_last_gap_cmdlist(),
                 (unsigned long)video_hw_last_gap_underruns());
        fs_core0_log(line);
        last_long_gaps = gaps;
    }
}

/* One line per boot, so the log shows whether the board restarted (and
 * why) between status lines. */
static void log_boot(uint32_t sys_khz, bool was_watchdog) {
    char line[160];
#if defined(SPICOMPUTER_HAS_HDMI)
    uint32_t div, hstx, pixel;
    bool warn;
    video_hw_clock_info(&div, &hstx, &pixel, &warn);
    snprintf(line, sizeof(line),
             "boot: sys=%lu kHz hstx=%lu Hz pixel=%lu Hz warn=%d "
             "watchdog=%d",
             (unsigned long)sys_khz, (unsigned long)hstx,
             (unsigned long)pixel, warn ? 1 : 0, was_watchdog ? 1 : 0);
#else
    (void)sys_khz;
    snprintf(line, sizeof(line), "boot: watchdog=%d", was_watchdog ? 1 : 0);
#endif
    fs_core0_log(line);
}

static void print_boot_info(void) {
    uint32_t sys = clock_get_hz(clk_sys);
    printf("System Clock Frequency is %d Hz\n", sys);
    printf("Peripheral Clock Frequency is %d Hz\n", clock_get_hz(clk_peri));
    printf("USB Clock Frequency is %d Hz\n", clock_get_hz(clk_usb));
#if defined(PICO_RP2350)
    if (sys / 1000 != SPICOMPUTER_SYS_CLOCK_KHZ) {
        printf("overclock: %u kHz is not exactly achievable; using %u kHz\n",
               (unsigned)SPICOMPUTER_SYS_CLOCK_KHZ, (unsigned)(sys / 1000));
    }
#endif

#if defined(SPICOMPUTER_HAS_HDMI)
    uint32_t div, hstx, pixel;
    bool warn;
    video_hw_clock_info(&div, &hstx, &pixel, &warn);
    printf("HSTX: clk_sys %u / %u = %u Hz (pixel clock %u Hz)\n",
           (unsigned)sys, (unsigned)div, (unsigned)hstx, (unsigned)pixel);
    if (warn) {
        printf("HSTX: warning: clk_sys does not divide to a 640x480@60 "
               "pixel clock; expect ~%u Hz refresh\n",
               (unsigned)(pixel / (800u * 525u)));
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

    print_boot_info();
    if (was_watchdog) {
        printf("Rebooted by Watchdog!\n");
    }

    /* Filesystem: the SD card is on this core now, so the Lua bindings
     * call straight into the FatFs layer with no cross-core round trip.
     * The video core claimed its DMA channels and DMA_IRQ_2 before this
     * core started, so the SD driver gets its own. */
    rpc_set_local_handler(fs_service);
#if defined(SPICOMPUTER_VIDEO_TEST_PATTERN)
    /* The pattern build deliberately performs no SD work: this isolates
     * the video core from card-init traffic, DMA and XIP activity. */
    printf("test pattern build: SD mount skipped\n");
#else
    if (!fs_core0_mount()) {
        printf("SD card not mounted\n");
    }
#endif
    /* Record the clock/display setup and whether this run followed a
     * watchdog reset; the log is appended, so it survives resets. */
    log_boot(clock_get_hz(clk_sys) / 1000, was_watchdog);
    boot_signal(6); /* card stage complete */

    /* Input subsystem: 1 kHz matrix and joystick scanning. This claims
     * the alarm pool (and its IRQ) for this core. */
    input_hw_init();

#if defined(SPICOMPUTER_VIDEO_TEST_PATTERN)
    /* Diagnostic build: the scanout draws a test pattern, so no program
     * is loaded (the SD mount above still reports the card). */
    printf("test pattern build: boot program not loaded\n");
#else
    if (!program_boot(BOOT_SCRIPT, NULL)) {
        printf("Boot failed (no SD card?)\n");
    }
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

    for (;;) {
        /* Drain input, run due timers, otherwise tick. */
        program_scheduler_step();

        uint64_t now = time_us_64();
        uint32_t frames = g_system_state.video_frame_count;
        if (frames != last_frames) {
            last_frames = frames;
            last_frame_us = now;
        }

#if defined(SPICOMPUTER_HAS_HDMI)
        /* Two liveness gates: this loop is running (a stuck VM or a
         * deadlock stops the scheduler step above) and the display is
         * producing frames. On boards without HSTX only the first
         * applies; the frame counter never moves there. */
        bool video_alive = (now - boot_us < VIDEO_BOOT_GRACE_US) ||
                           (now - last_frame_us < VIDEO_STALL_LIMIT_US);
        if (video_alive) {
            watchdog_update();
        }
#else
        (void)boot_us;
        (void)last_frame_us;
        watchdog_update();
#endif

        if (now - stats_us >= LOG_PERIOD_US) {
            stats_us = now;
            boot_signal_tick(); /* alive: status-LED blink */
            log_status(now, frames);
#if defined(SPICOMPUTER_HAS_HDMI)
            uint32_t underruns = video_hw_underruns();
            printf("video: frame %lu scanline %lu underruns %lu (+%lu)\n",
                   (unsigned long)frames,
                   (unsigned long)video_hw_scanline(),
                   (unsigned long)underruns,
                   (unsigned long)(underruns - last_underruns));
            printf("video: black %lu snapfail %lu skew %lu(%lu) gap %lu us "
                   "long %lu empty %lu wof %lu\n",
                   (unsigned long)video_hw_black_rows(),
                   (unsigned long)video_hw_snapshot_fails(),
                   (unsigned long)video_hw_skews(),
                   (unsigned long)video_hw_last_frame_steps(),
                   (unsigned long)video_hw_gap_max_us(),
                   (unsigned long)video_hw_long_gaps(),
                   (unsigned long)video_hw_fifo_empty(),
                   (unsigned long)video_hw_fifo_wofs());
            last_underruns = underruns;
#endif
        }
    }
}
