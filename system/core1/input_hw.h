/* core1/input_hw.h
 *
 * Hardware glue for the input subsystem (Phase 3), on the OS core: GPIO
 * wiring for the key matrix and joysticks and the 1 kHz input tick
 * timer. All event production happens here, inside the timer IRQ, so the
 * queue has a single producer.
 */
#pragma once

void input_hw_init(void);
