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

/* Main-loop render pump (core 0 only): renders rows into the scanout
 * ring when the DMA IRQ has posted something. Must be called with
 * interrupts enabled so the DMA IRQ can preempt it: rendering inside
 * an IRQ would block the post past the HSTX FIFO's ~1.3 us of buffered
 * pixels and drop the monitor's sync. */
void video_hw_poll(void);

/* Colour chequerboard test pattern. While enabled the scanout ignores
 * the program screen slots and draws a fixed 8x6 board of saturated
 * colours (at the 320x240 logical geometry, 2x scaled like every mode),
 * so the HDMI path can be judged with no card, no Lua and no op queue
 * involved. Callable from either core: the flag is latched at the next
 * frame boundary, so the switch never tears a frame. The OS core turns
 * it on when the card is missing or the boot program cannot start; the
 * SPICOMPUTER_CHEQUERBOARD build forces it on and skips the card. */
void video_hw_set_test_pattern(bool on);
bool video_hw_test_pattern(void);

/* Frame counter (incremented by the scanout at the end of each active
 * region). Exposed for WaitVSync and diagnostics. */
uint32_t video_hw_frame_count(void);

/* Current output scanline (0..524) and ring underrun count: bring-up
 * diagnostics. Safe to call from the OS core (plain reads). */
uint32_t video_hw_scanline(void);
uint32_t video_hw_underruns(void);

/* Bring-up diagnostics for the "picture drops out but the system keeps
 * running" class of faults. All are cumulative counters since boot:
 *
 *   skews/last    - frames whose DMA step count was not exactly
 *                   (blanking + 480*2) steps, with the last bad count
 *                   (the sequencer lost or gained a step, so the line
 *                   structure shifted)
 *   gap_max_us    - worst interval between two DMA completion IRQs; one
 *                   line is ~31.7 us, so far above that is a stalled
 *                   stream (frozen picture / lost TMDS lock)
 *   long_gaps     - completions more than 64 us after the previous one
 *   fifo_empty    - completions that found the HSTX FIFO empty
 *   fifo_wofs     - HSTX FIFO write-overflow events (a dropped word)
 */
uint32_t video_hw_skews(void);
uint32_t video_hw_last_frame_steps(void);
uint32_t video_hw_gap_max_us(void);
uint32_t video_hw_long_gaps(void);
uint32_t video_hw_fifo_empty(void);
uint32_t video_hw_fifo_wofs(void);

/* Details of the most recent >64 us gap: its length, the frame and
 * output scanline it ended on, the HSTX FIFO level seen then (0 = the
 * FIFO had run dry), whether the next post is a command list, and the
 * ring underrun count at that moment. Lets the OS core log where in the
 * frame a stall happened. */
uint32_t video_hw_last_gap_us(void);
uint32_t video_hw_last_gap_frame(void);
uint32_t video_hw_last_gap_line(void);
uint32_t video_hw_last_gap_fifo_level(void);
uint32_t video_hw_last_gap_cmdlist(void);
uint32_t video_hw_last_gap_underruns(void);

/* HSTX clock information recorded by video_hw_init(), for the OS core to
 * print: the clk_hstx source divisor, the resulting peripheral clock and
 * the pixel clock (clk_hstx / 5). `*warn` is set when clk_sys did not
 * divide to the 25.2 MHz pixel clock (an overclock off the exact list). */
void video_hw_clock_info(uint32_t *divisor, uint32_t *hstx_hz,
                         uint32_t *pixel_hz, bool *warn);
