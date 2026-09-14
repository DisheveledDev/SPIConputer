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
 * That leaves exactly one cross-core object: the video frame counter,
 * written by core 0's scanout at each vertical blank and read by core 1
 * (WaitVSync, the video-state retire queue and the watchdog gate). It is
 * a single word with a single writer, so a plain read is enough: the
 * cores share SRAM with no data caches between them.
 */
#pragma once

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
     * 1 (WaitVSync, the retire queue, the watchdog gate). This is the
     * only object the two cores share. Zero on boards and in builds with
     * no HSTX output. */
    volatile uint32_t video_frame_count;
} system_state_t;

extern system_state_t g_system_state;
