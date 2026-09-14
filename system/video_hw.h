/* video_hw.h — HDMI output over RP2350 HSTX.
 *
 * The implementation lives on the video core (core0/video_hw.c); this
 * header is shared because the OS core reads the frame counter, the
 * scanline and the clock information for WaitVSync, diagnostics and the
 * watchdog gate. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Initialise the HDMI/DVI output path on the product board. Called
 * from core 0 main() before core 1 starts; claims two DMA channels and
 * a DMA IRQ line, so call it before the SD driver initialises if the
 * channel numbers are to be predictable. */
void video_hw_init(void);

/* Frame counter (incremented by the scanout at the end of each active
 * region). Exposed for WaitVSync and diagnostics. */
uint32_t video_hw_frame_count(void);

/* Current output scanline (0..524) and ring underrun count: bring-up
 * diagnostics. Safe to call from the OS core (plain reads). */
uint32_t video_hw_scanline(void);
uint32_t video_hw_underruns(void);

/* HSTX clock information recorded by video_hw_init(), for the OS core to
 * print: the clk_hstx source divisor, the resulting peripheral clock and
 * the pixel clock (clk_hstx / 5). `*warn` is set when clk_sys did not
 * divide to a 640x480@60 pixel clock (an overclock off the exact list). */
void video_hw_clock_info(uint32_t *divisor, uint32_t *hstx_hz,
                         uint32_t *pixel_hz, bool *warn);
