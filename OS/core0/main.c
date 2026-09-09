/* core0/main.c
 *
 * Core 0: hardware core.
 *
 * Owns all hardware I/O: peripheral init, watchdog, and (from later
 * phases) the key matrix scan, joystick reading, RS232 link, SD card
 * (behind the RPC) and HDMI rendering. Core 1 runs the Lua engine and is
 * launched from here.
 *
 * Phase 0 scope: init + launch core 1 + watchdog feed loop.
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/spi.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"

#include "board_config.h"

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

int64_t alarm_callback(alarm_id_t id, void *user_data) {
    // Put your timeout handler code in here
    return 0;
}

static void init_uart_rs232(void) {
    uart_init(RS232_UART_INSTANCE, 115200);
    gpio_set_function(RS232_PIN_TX, GPIO_FUNC_UART);
    gpio_set_function(RS232_PIN_RX, GPIO_FUNC_UART);
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

    // Timer example code - This example fires off the callback after 2000ms
    add_alarm_in_ms(2000, alarm_callback, NULL, false);

    // Watchdog: fed from the loop below. (Later phases gate the feed on a
    // core 1 heartbeat so a stuck Lua VM also resets the system.)
    if (watchdog_caused_reboot()) {
        printf("Rebooted by Watchdog!\n");
    }
    watchdog_enable(2000, 1);
    watchdog_update();

    printf("System Clock Frequency is %d Hz\n", clock_get_hz(clk_sys));
    printf("USB Clock Frequency is %d Hz\n", clock_get_hz(clk_usb));

    init_uart_rs232();
    uart_puts(RS232_UART_INSTANCE, " Hello, UART!\n");

    // Hand the Lua engine to core 1. All IRQs stay on core 0.
    multicore_launch_core1(core1_entry);

    for (;;) {
        watchdog_update();
        sleep_ms(1000);
    }
}
