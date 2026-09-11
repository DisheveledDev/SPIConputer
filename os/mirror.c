/* mirror.c — see mirror.h */
#include "mirror.h"

#include <stdio.h>

int mirror_encode_frame(const video_state_t *v, char *buf, size_t cap) {
    size_t used = 0;
    const char *prefix = "data=";

    if (cap < sizeof("data=\n")) {
        return -1;
    }
    for (size_t i = 0; prefix[i]; i++) {
        buf[used++] = prefix[i];
    }
    int cols = video_char_cols(v);
    int rows = video_char_rows(v);
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            int n = snprintf(buf + used, cap - used, "%s%u",
                             (y == 0 && x == 0) ? "" : ",", 
                             v->char_map[y * cols + x]);
            if (n < 0 || (size_t)n >= cap - used) {
                return -1;
            }
            used += (size_t)n;
        }
    }
    if (used + 1 >= cap) {
        return -1;
    }
    buf[used++] = '\n';
    buf[used] = '\0';
    return (int)used;
}

int mirror_encode_tile(uint8_t index, const uint8_t rows[8], char *buf,
                       size_t cap) {
    int n = snprintf(buf, cap,
                     "tile=%u,%u,%u,%u,%u,%u,%u,%u,%u\n", index, rows[0],
                     rows[1], rows[2], rows[3], rows[4], rows[5], rows[6],
                     rows[7]);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return n;
}
