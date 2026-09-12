/* core0/video_hw.c
 *
 * HDMI output over HSTX (Phase 7 bring-up). The scanline renderer
 * (render.c) already produces 640x480 RGB888 scanlines from the
 * current video state; this file is the landing spot for the HSTX/DVI
 * pipeline that consumes them.
 *
 * Bring-up plan (see AGENTS.md, Phase 7):
 *   1. Upgrade to SDK >= 2.4.0, which adds hardware_hstx (SDK 2.3.1
 *      has only the register headers).
 *   2. Port pico-examples hstx/dvi_out_hstx_encoder (8-bit TMDS
 *      encoder + HSTX command stream + DMA) as the base.
 *   3. Configure clk_hstx for the 25.175 MHz DVI pixel clock, fixed
 *      640x480@60 timing, one configuration for the life of the
 *      process.
 *   4. Feed render_line() output into the encoder scanline buffers,
 *      then add the double-buffered char/attr map swap at vsync.
 *
 * Until then the product board runs headless (serial mirror only).
 */
#include "video_hw.h"

#include <stdio.h>
#include "pico/stdlib.h"

void video_hw_init(void) {
    printf("HDMI bring-up pending (Phase 7): serial mirror active\n");
}
