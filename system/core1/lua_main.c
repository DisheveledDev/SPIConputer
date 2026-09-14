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
#include "scanout.h"
#include "system_state.h"
#include "video_hw.h"

// Boot program loaded from the SD card root at startup (starts the
// shell, os.lua, in the foreground).
#define BOOT_SCRIPT "core/boot.lua"

/* A frozen display is as fatal as a frozen VM: if the frame counter has
 * not moved for this long, stop feeding the watchdog. */
#define VIDEO_STALL_LIMIT_US 250000

/* Bring-up status period (SPICOMPUTER_LOG_PERIOD_MS, default 1000).
 * Video diagnostics are printed to USB stdio only; they are never
 * written to the SD card. */
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
        printf("HSTX: warning: clk_sys does not divide to a 25.2 MHz "
               "pixel clock; expect ~%u Hz refresh\n",
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

    print_boot_info();
    if (was_watchdog) {
        printf("Rebooted by Watchdog!\n");
    }

    /* Filesystem: the SD card is on this core now, so the Lua bindings
     * call straight into the FatFs layer with no cross-core round trip.
     * The video core claimed its DMA channels and DMA_IRQ_2 before this
     * core started, so the SD driver gets its own. */
    rpc_set_local_handler(fs_service);
    bool card_ok = false;
#if defined(SPICOMPUTER_VIDEO_TEST_PATTERN) || defined(SPICOMPUTER_CHEQUERBOARD)
    /* Test pattern builds deliberately perform no SD work: this isolates
     * the video core from card-init traffic, DMA and XIP activity. */
    printf("test pattern build: SD mount skipped\n");
#else
    card_ok = fs_core0_mount();
    if (!card_ok) {
        printf("SD card not mounted\n");
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
        printf("Boot failed: showing the chequerboard test pattern\n");
    } else {
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
#else
        (void)boot_us;
        (void)last_frame_us;
        watchdog_update();
#endif

        if (now - stats_us >= LOG_PERIOD_US) {
            stats_us = now;
            boot_signal_tick(); /* alive: status-LED blink */
#if defined(SPICOMPUTER_HAS_HDMI)
            uint32_t underruns = video_hw_underruns();
            /* Uptime makes a reset obvious in the log: it drops back to 0
             * (and "Rebooted by Watchdog!" appears above it). */
            printf("video: up %lus frame %lu scanline %lu underruns %lu "
                   "(+%lu)\n",
                   (unsigned long)((now - boot_us) / 1000000u),
                   (unsigned long)frames,
                   (unsigned long)video_hw_scanline(),
                   (unsigned long)underruns,
                   (unsigned long)(underruns - last_underruns));
            printf("video: skew %lu(%lu) gap %lu us long %lu empty %lu "
                   "wof %lu late %lu fifo_min %lu\n",
                   (unsigned long)video_hw_skews(),
                   (unsigned long)video_hw_last_frame_steps(),
                   (unsigned long)video_hw_gap_max_us(),
                   (unsigned long)video_hw_long_gaps(),
                   (unsigned long)video_hw_fifo_empty(),
                   (unsigned long)video_hw_fifo_wofs(),
                   (unsigned long)video_hw_late_posts(),
                   (unsigned long)video_hw_fifo_min());
            last_underruns = underruns;
#endif
        }
    }
}
