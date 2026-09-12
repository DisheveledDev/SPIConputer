/* core0/main.c
 *
 * Core 0: hardware core.
 *
 * Owns all hardware I/O: peripheral init, the SD card (FatFs runs here,
 * behind the RPC), input scanning (1 kHz IRQ), the watchdog, and (later
 * phases) HDMI rendering. Core 1 runs the Lua engine and is launched
 * from here; it reaches the SD card only through RPC requests serviced
 * in the loop below.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/sync.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"

#include "board_config.h"
#include "input_hw.h"
#include "fs_core0.h"
#include "rpc.h"
#include "system_state.h"
#include "video_hw.h"

/* Core 1 entry point (defined in core1/lua_main.c) */
extern void core1_entry(void);

/* Placeholder display SPI (the generated example's SPI0 wiring).
 * GP16-19 are HSTX lanes on RP2350, so this only exists on boards
 * without HDMI. */
#if !defined(SPICOMPUTER_HAS_HDMI)
#define SPI_PORT spi0
#define PIN_MISO 16
#define PIN_CS   17
#define PIN_SCK  18
#define PIN_MOSI 19
#endif

/* ------------------------------------------------------------------ */
/* RPC wakeup: core 1 blocks on this semaphore, core 0 releases it
 * after completing a request. Cross-core semaphores are multicore-safe
 * in the SDK. */
/* ------------------------------------------------------------------ */

static semaphore_t s_rpc_sem;

static void rpc_wait(void) {
    sem_acquire_blocking(&s_rpc_sem);
}

static void rpc_signal(void) {
    sem_release(&s_rpc_sem);
}

int main(void)
{
    stdio_init_all();

#if !defined(SPICOMPUTER_HAS_HDMI)
    // Placeholder display SPI; superseded by HSTX/HDMI on the product board.
    spi_init(SPI_PORT, 1000 * 1000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_CS,   GPIO_FUNC_SIO);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    // Chip select is active-low, so we'll initialise it to a driven-high state
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);
#endif

    // Watchdog: fed from the loop below, gated on core 1's heartbeat
    // (a stuck Lua VM stops the feeds and resets the system).
    if (watchdog_caused_reboot()) {
        printf("Rebooted by Watchdog!\n");
    }
    watchdog_enable(2000, 1);
    watchdog_update();

    printf("System Clock Frequency is %d Hz\n", clock_get_hz(clk_sys));
    printf("USB Clock Frequency is %d Hz\n", clock_get_hz(clk_usb));

    // SD card + FatFs: core 0 only, from here on.
    if (!fs_core0_mount()) {
        printf("SD card not mounted\n");
    }

    // Input subsystem: 1 kHz matrix and joystick scanning.
    input_hw_init();

    // HDMI output (Phase 7 bring-up; stub until HSTX lands).
#if defined(SPICOMPUTER_HAS_HDMI)
    video_hw_init();
#endif

    // RPC transport: core 1 blocks on the semaphore, core 0 wakes it.
    sem_init(&s_rpc_sem, 0, 1);
    rpc_bind_wait(rpc_wait);
    rpc_bind_signal(rpc_signal);

    // Hand the Lua engine to core 1. All IRQs stay on core 0.
    multicore_launch_core1(core1_entry);

    // Main loop: service filesystem RPCs and feed the watchdog on
    // evidence of progress. Progress is: a core 1 heartbeat advance,
    // an RPC serviced, or still within the boot grace period.
    uint32_t last_heartbeat = 0;
    uint64_t boot_us = time_us_64();
    for (;;) {
        bool progress = false;

        if (fs_core0_service()) {
            progress = true;
        }

        uint32_t hb = atomic_load_explicit(&g_system_state.heartbeat,
                                           memory_order_relaxed);
        if (hb != last_heartbeat) {
            last_heartbeat = hb;
            progress = true;
        }
        if (time_us_64() - boot_us < 5000000) {
            progress = true; /* boot grace: cover SD init + boot.lua */
        }

        if (progress) {
            watchdog_update();
        }
        tight_loop_contents();
    }
}
