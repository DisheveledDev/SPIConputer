/* boot_signal.h
 *
 * Boot progress signalling for boards with a debug LED
 * (SPICOMPUTER_HAS_BOOT_LED, the pico2 dev board's onboard LED). On the
 * product board - where the same pin is a keyboard column - every
 * function here is a no-op, so callers can sprinkle them through the
 * boot path unconditionally.
 *
 * The LED is the only boot channel that works when USB is dead or the
 * firmware hangs before stdio, so each stage of the boot gets a code:
 *
 *   1  core 0: main() entered
 *   2  core 0: system clock applied (and flash timing rescaled)
 *   3  core 0: video output up
 *   4  core 0: core 1 launched
 *   5  core 1: stdio up (USB should be enumerating by now)
 *   6  core 1: SD mount attempted
 *   7  core 1: scheduler loop reached (heartbeat starts)
 *
 * A steady 1 Hz blink means the OS core is alive in its scheduler loop.
 */
#pragma once

/* Blink `code` times. Blocking (a few hundred ms per code), safe to call
 * before the scheduler runs: interrupts may be disabled and nothing else
 * is using the pin. */
void boot_signal(unsigned code);

/* One non-blocking toggle, for the heartbeat in the scheduler loop. */
void boot_signal_tick(void);
