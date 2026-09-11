/* mirror.h
 *
 * Serial mirror text encoder (Phase 1, firmware side of the protocol
 * the macOS terminal app parses). Modes 0/1 only (40x30).
 */
#pragma once

#include <stddef.h>

#include "video.h"

/* Encode one full frame as a `data=` line (decimal tile values,
 * row-major, LF-terminated). Returns bytes written, or -1 if the
 * buffer is too small (worst case ~4.3 KB for 1200 values). */
int mirror_encode_frame(const video_state_t *v, char *buf, size_t cap);

/* Encode one `tile=` line for a custom tile definition. */
int mirror_encode_tile(uint8_t index, const uint8_t rows[8], char *buf,
                       size_t cap);
