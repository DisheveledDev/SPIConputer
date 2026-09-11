/* core0/video_hw.h — HDMI output (Phase 7 bring-up) */
#pragma once

/* Initialise the HDMI/DVI output path on the product board. Called
 * from core 0 main() before core 1 starts. */
void video_hw_init(void);
