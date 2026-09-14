/* system_state.h
 *
 * The state shared between the two cores. After the core split the
 * division of labour is:
 *
 *   - core 0 (video core): the HSTX scanout only. Its DMA/render IRQs
 *     run there and nothing else executes on that core.
 *   - core 1 (OS core): everything else, filesystem (FatFs + SD), input
 *     scanning, stdio, the watchdog and the Lua scheduler. Input, the
 *     filesystem and the scheduler are same-core, so they need no
 *     sharing primitives at all.
 *
 * That leaves two cross-core objects: the video frame counter, written
 * by core 0's scanout at each vertical blank and read by core 1
 * (WaitVSync, the video-state retire queue and the watchdog gate), and
 * the test pattern request, written once by core 1 at boot and read by
 * core 0 at each frame boundary. Both are single words with a single
 * writer, so a plain read is enough: the cores share SRAM with no data
 * caches between them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input.h"

typedef struct {
    /* Input event ring. Producer (the 1 kHz IRQ) and consumer (the
     * scheduler) are both on the OS core now, so this is core-1-local
     * state; it lives here because external builds (the simulator, the
     * IDE) reach it through this object. The ring indices are plain
     * words - see input.h. */
    input_queue_t input;

    /* Completed video frames. Written by core 0's scanout, read by core
     * 1 (WaitVSync, the retire queue, the watchdog gate). Zero on boards
     * and in builds with no HSTX output. */
    volatile uint32_t video_frame_count;

    /* Draw the bring-up test pattern instead of the program screen.
     * Raised by core 1 during boot when there is no program to run (SD
     * card not mounted, or the boot script missing) and initialised true
     * in diagnostic builds (SPICOMPUTER_VIDEO_TEST_PATTERN), so a bare
     * board still shows something that exercises every HSTX lane for
     * wiring checks. Core 0 latches it at each frame boundary. */
    volatile bool video_pattern_request;
} system_state_t;

extern system_state_t g_system_state;
