/* core0/input_hw.h
 *
 * Core 0 hardware glue for the input subsystem (Phase 3): GPIO wiring
 * for the key matrix and joysticks, the 1 kHz input tick timer, and
 * RS232 RX draining. All event production happens on core 0, inside
 * the timer IRQ, so the shared queue has a single producer.
 */
#pragma once

void input_hw_init(void);
