/* core0/main.c
 *
 * Core 0: the video core. After the core split this core does one thing:
 * the HSTX scanout. It brings the display up (TMDS expansion, the DMA
 * ping/pong chain, the render ISR) and then idles in WFI while those
 * IRQs feed the display from g_current_video.
 *
 * Everything else - stdio, the SD card and FatFs, input scanning, the
 * watchdog and the Lua scheduler - runs on core 1 (core1/lua_main.c),
 * which this file launches. Nothing here may touch stdio (the OS core
 * owns it) and nothing here may use the alarm pool: sleep_ms()/timers
 * would claim the shared timer IRQ for this core and steal the OS core's
 * input tick. The one cross-core object is the frame counter in
 * g_system_state, which the scanout writes.
 */

#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/spi.h"
#include "hardware/structs/qmi.h"
#include "hardware/vreg.h"
#include "hardware/xip_cache.h"

#include "board_config.h"
#include "boot_signal.h"
#include "video_hw.h"

/* Core 1 entry point (defined in core1/lua_main.c) */
extern void core1_entry(void);

/* Core 1's stack. The SDK's default core-1 stack lives in SCRATCH_X,
 * which is only 4 KB on this chip; the OS core runs stdio, FatFs and the
 * Lua VM, so it gets a larger stack in main SRAM instead. */
#define CORE1_STACK_BYTES (16 * 1024)
static uint32_t __attribute__((aligned(8)))
    s_core1_stack[CORE1_STACK_BYTES / sizeof(uint32_t)];

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
/* CPU overclock                                                      */
/*                                                                    */
/* RP2350 only: the QMI flash timing and the HSTX clock divider it     */
/* has to work with are RP2350 peripherals.                           */
/* ------------------------------------------------------------------ */

#if defined(PICO_RP2350)

/* 150 MHz is the stock clock, so any overclock needs more core voltage.
 * 1.25 V is comfortable for 252 MHz and keeps the regulator well inside
 * its limits; raise this if SPICOMPUTER_SYS_CLOCK_KHZ is raised (378 MHz
 * wants 1.30 V, the SDK's maximum). */
#if SPICOMPUTER_SYS_CLOCK_KHZ > 300000
#define SPICOMPUTER_OC_VOLTAGE VREG_VOLTAGE_1_30
#else
#define SPICOMPUTER_OC_VOLTAGE VREG_VOLTAGE_1_25
#endif

/* The QSPI read command the boot stage selected in QMI M0 (0x03, 0x0B,
 * 0xBB or 0xEB). */
static uint32_t xip_read_cmd(void) {
    return (qmi_hw->m[0].rcmd & QMI_M0_RCMD_PREFIX_BITS) >>
           QMI_M0_RCMD_PREFIX_LSB;
}

static bool xip_read_mode(uint32_t cmd, bootrom_xip_mode_t *mode) {
    switch (cmd) {
        case 0x03: *mode = BOOTROM_XIP_MODE_03H_SERIAL; return true;
        case 0x0b: *mode = BOOTROM_XIP_MODE_0BH_SERIAL; return true;
        case 0xbb: *mode = BOOTROM_XIP_MODE_BBH_DUAL; return true;
        case 0xeb: *mode = BOOTROM_XIP_MODE_EBH_QUAD; return true;
        default: return false;
    }
}

/* Scale the flash clock with clk_sys.
 *
 * The boot stage runs the QSPI flash at clk_sys/PICO_FLASH_SPI_CLKDIV
 * (150/2 = 75 MHz on this board). Raising clk_sys without touching that
 * would overclock the flash, so the flash divider and its sampling delay
 * are re-programmed first, while the system clock is still low.
 *
 * The preferred path hands the work to the bootrom
 * (rom_flash_select_xip_read_mode): it re-applies the mode's complete
 * timing table (divider, cooldown, RX delay) for the new divider - the
 * same table it used to bring the flash up at boot - so the sampling
 * point is right rather than approximated. The fallback scales the
 * divider and keeps the boot RXDELAY/divider ratio, which is the
 * proportional part of the same calculation (RXDELAY is in half clk_sys
 * cycles, so the delay for a given sample point scales with the
 * divider).
 *
 * This must run from SRAM: XIP timing is in flux across the change, and
 * the cache is flushed after. Interrupts are still off at this point. */
static void __no_inline_not_in_flash_func(flash_scale_clock)(
    uint32_t old_khz, uint32_t new_khz) {
    uint32_t timing = qmi_hw->m[0].timing;
    uint32_t div =
        (timing & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;

    /* Round the divider up so the flash never runs faster than at boot. */
    uint32_t new_div = (div * new_khz + old_khz - 1) / old_khz;
    if (new_div < 1) new_div = 1;
    if (new_div > 0xffu) new_div = 0xffu;

    bootrom_xip_mode_t mode;
    if (xip_read_mode(xip_read_cmd(), &mode)) {
        rom_flash_select_xip_read_mode(mode, (uint8_t)new_div);
        xip_cache_invalidate_all();
        return;
    }

    /* Unknown read mode: scale the divider and the RX delay ourselves. */
    uint32_t rx =
        (timing & QMI_M0_TIMING_RXDELAY_BITS) >> QMI_M0_TIMING_RXDELAY_LSB;
    uint32_t new_rx = div ? (rx * new_div + div - 1) / div : rx;
    if (new_rx > 7u) new_rx = 7u;
    qmi_hw->m[0].timing =
        (timing & ~(QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS)) |
        (new_div << QMI_M0_TIMING_CLKDIV_LSB) |
        (new_rx << QMI_M0_TIMING_RXDELAY_LSB);
    xip_cache_invalidate_all();
}

/* Bring clk_sys up to SPICOMPUTER_SYS_CLOCK_KHZ, or down to 126 MHz if
 * that is not achievable, or leave it alone. Returns the clock in use;
 * the OS core reports the result against the request (it owns stdio). */
static uint32_t apply_sys_clock(void) {
    uint32_t old_khz = clock_get_hz(clk_sys) / 1000;
    uint32_t want = SPICOMPUTER_SYS_CLOCK_KHZ;
    uint vco, postdiv1, postdiv2;

    if (!check_sys_clock_khz(want, &vco, &postdiv1, &postdiv2)) {
        want = 126000; /* the exact 25.2 MHz pixel clock source */
        if (!check_sys_clock_khz(want, &vco, &postdiv1, &postdiv2)) {
            return clock_get_hz(clk_sys);
        }
    }
    if (want > old_khz) {
        vreg_set_voltage(SPICOMPUTER_OC_VOLTAGE);
        /* Let the regulator settle. A busy wait, not sleep_ms(): sleeping
         * would claim the shared timer IRQ for this core and starve the
         * OS core's 1 kHz input tick. */
        busy_wait_us(10000);
        flash_scale_clock(old_khz, want);
    }
    /* Note: this also moves clk_peri to the 48 MHz USB clock (the SDK
     * default for a PLL change), which is what the SD SPI driver's
     * divider is computed from. */
    set_sys_clock_pll(vco, postdiv1, postdiv2);
    return clock_get_hz(clk_sys);
}

#endif /* PICO_RP2350 */

int main(void)
{
    boot_signal(1); /* core 0 alive */

    /* Before anything that depends on the clock: the HSTX divider and the
     * OS core's stdio baud and SPI dividers are derived from it. */
#if defined(PICO_RP2350)
    apply_sys_clock();
#endif
    boot_signal(2); /* clock + flash timing applied */

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

    /* The display first, so it is live (showing black) before the OS core
     * mounts the card and the first program exists. This also claims the
     * video DMA channels and DMA_IRQ_2 before the SD driver on core 1
     * takes its own channels and DMA_IRQ_0. */
    video_hw_init();
    boot_signal(3); /* display up */

    /* Hand the rest of the system to the OS core. */
    multicore_launch_core1_with_stack(core1_entry, s_core1_stack,
                                      sizeof(s_core1_stack));
    boot_signal(4); /* OS core launched */

    /* Nothing else to do here: the scanout's DMA IRQ posts the next
     * buffer and raises a flag; rendering happens in this loop so the
     * IRQ can always preempt it. WFI (rather than a spin) keeps this
     * core out of the way of the DMA and the XIP cache - and keeps the
     * overclocked core cool - while the OS core does the work. */
    for (;;) {
        video_hw_poll();
        __wfi();
    }
}
