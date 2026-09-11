/* core0/serial_mirror.c
 *
 * Serial mirror: streams the current video state (modes 0/1, 40x30)
 * to the RS232 terminal application using the text frame protocol
 * (see AGENTS.md; the Swift app lives in the sibling `terminal/` folder).
 * Owned by core 0; reads
 * g_current_video (written by core 1's screen module) and tolerates
 * mid-frame updates (bounded tearing, see AGENTS.md Q18).
 *
 * Headers and tile definitions are queued when the video state's
 * version changes; the `data=` frame line is queued continuously,
 * throttled to ~3 fps. Transmission is chunked so the main loop stays
 * responsive (RPC latency, watchdog).
 */
#include "serial_mirror.h"

#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/uart.h"

#include "board_config.h"
#include "mirror.h"
#include "os_time.h"
#include "system_state.h"
#include "video.h"

/* ~3 fps at 115200 baud (full data= line is ~4.2 KB). */
#define MIRROR_FRAME_PERIOD_US 330000

/* Bytes sent per poll() call: keeps RPC/watchdog latency low. */
#define MIRROR_CHUNK 256

/* Headers + tile lines + one full data= frame. */
#define MIRROR_TX_SIZE 5500

static char s_tx[MIRROR_TX_SIZE];
static size_t s_tx_len;
static size_t s_tx_pos;
static uint32_t s_last_version;
static int s_last_mode;
static uint64_t s_last_frame_us;
static uint8_t s_tiles_snapshot[256][8];
static uint8_t s_defined_snapshot[256];

static void tx_begin(void) {
    s_tx_len = 0;
    s_tx_pos = 0;
}

/* Append a line (must be LF-terminated) to the pending transmission.
 * Silently dropped if the buffer is full. */
static void tx_line(const char *line) {
    size_t n = strlen(line);
    if (s_tx_len + n < sizeof(s_tx)) {
        memcpy(s_tx + s_tx_len, line, n);
        s_tx_len += n;
    }
}

/* Send the next chunk; returns true when there is more to send. */
static bool tx_flush_chunk(void) {
    size_t n = s_tx_len - s_tx_pos;
    if (n == 0) {
        return false;
    }
    if (n > MIRROR_CHUNK) {
        n = MIRROR_CHUNK;
    }
    for (size_t i = 0; i < n; i++) {
        uart_putc_raw(RS232_UART_INSTANCE, s_tx[s_tx_pos + i]);
    }
    s_tx_pos += n;
    if (s_tx_pos >= s_tx_len) {
        tx_begin();
        return false;
    }
    return true;
}

static void send_headers_if_changed(const video_state_t *v) {
    if (v->mode != s_last_mode) {
        s_last_mode = v->mode;
        tx_line("resolution=40x30\n");
        tx_line("foreground=yellow\n");
        tx_line("background=darkblue\n");
    }
    /* Tile diffs: send only redefined tiles that changed (or appeared). */
    for (int i = 0; i < 256; i++) {
        if (v->tile_defined[i] &&
            (!s_defined_snapshot[i] ||
             memcmp(s_tiles_snapshot[i], v->tiles[i], 8) != 0)) {
            char line[64];
            if (mirror_encode_tile((uint8_t)i, (const uint8_t *)v->tiles[i],
                                   line, sizeof(line)) > 0) {
                tx_line(line);
            }
            memcpy(s_tiles_snapshot[i], v->tiles[i], 8);
        }
        s_defined_snapshot[i] = v->tile_defined[i];
    }
}

void serial_mirror_init(void) {
    tx_begin();
    s_last_version = 0xffffffffu; /* force headers on first poll */
    s_last_mode = -1;
    s_last_frame_us = 0;
    memset(s_tiles_snapshot, 0, sizeof(s_tiles_snapshot));
    memset(s_defined_snapshot, 0, sizeof(s_defined_snapshot));
}

void serial_mirror_poll(void) {
    /* Keep draining the current transmission first. */
    if (tx_flush_chunk()) {
        return;
    }

    const video_state_t *v = g_current_video;
    if (!v || (v->mode != VIDEO_MODE_TEXT40 && v->mode != VIDEO_MODE_TEXT40C)) {
        return; /* mirror only exists for 40x30 text modes */
    }

    if (v->version != s_last_version) {
        s_last_version = v->version;
        send_headers_if_changed(v);
    }

    uint64_t now = os_time_us();
    if (now - s_last_frame_us < MIRROR_FRAME_PERIOD_US) {
        return;
    }
    s_last_frame_us = now;

    /* Full frame appended to whatever headers are pending. */
    int n = mirror_encode_frame(v, s_tx + s_tx_len, sizeof(s_tx) - s_tx_len);
    if (n > 0) {
        s_tx_len += (size_t)n;
        s_tx_pos = 0;
    }
}
