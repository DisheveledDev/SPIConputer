/* boot_signal.c — see boot_signal.h */
#include "boot_signal.h"

#include "board_config.h"

#if defined(SPICOMPUTER_HAS_BOOT_LED)

#include "pico/stdlib.h"

/* Set to true after the first boot_signal() so a repeated call (e.g. a
 * watchdog reboot) still shows its code. */
static bool s_inited;

static void led_init(void) {
    if (!s_inited) {
        gpio_init(SPICOMPUTER_BOOT_LED_PIN);
        gpio_set_dir(SPICOMPUTER_BOOT_LED_PIN, GPIO_OUT);
        s_inited = true;
    }
}

void boot_signal(unsigned code) {
    led_init();
    /* Blocking, and busy-waiting: this runs before the scheduler and
     * possibly before the alarm pool exists on this core. */
    for (unsigned i = 0; i < code; i++) {
        gpio_put(SPICOMPUTER_BOOT_LED_PIN, 1);
        busy_wait_us(60000);
        gpio_put(SPICOMPUTER_BOOT_LED_PIN, 0);
        busy_wait_us(200000);
    }
    busy_wait_us(600000); /* gap between codes */
}

void boot_signal_tick(void) {
    led_init();
    gpio_xor_mask(1u << SPICOMPUTER_BOOT_LED_PIN);
}

#else

void boot_signal(unsigned code) {
    (void)code;
}

void boot_signal_tick(void) {}

#endif
