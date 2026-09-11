/* protocol.h
 *
 * Line-oriented text protocol parser for the SPIComputer OS serial
 * mirror. Device -> host records (see AGENTS.md, "RS232 Development
 * Interface"):
 *
 *   resolution=40x30
 *   foreground=yellow
 *   background=darkblue
 *   tile=65,0,1,2,3,4,5,6,7
 *   data=0,0,0,36,38,41,87,0,0,76,98,...
 *
 * One record per line; the `data=` line is one full frame. Values are
 * decimal or 0x hex and are cast to uchar. Tile rows use the ROM font
 * convention: bit 0 is the leftmost pixel. The parser is incremental:
 * feed arbitrary byte chunks, drain one record per proto_parse() call.
 */
#ifndef SPITERM_PROTOCOL_H
#define SPITERM_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* Worst case frame: 80x60 cells. */
#define PROTO_MAX_FRAME_CELLS (80 * 60)

/* Longest record: a data= line for 80x60 cells, ~4 bytes per value. */
#define PROTO_MAX_LINE 8192

typedef enum {
    PROTO_REC_NONE = 0,
    PROTO_REC_RESOLUTION, /* u.res.w, u.res.h */
    PROTO_REC_FOREGROUND, /* u.colour.r/g/b */
    PROTO_REC_BACKGROUND, /* u.colour.r/g/b */
    PROTO_REC_TILE,       /* u.tile.index, u.tile.rows[8] */
    PROTO_REC_DATA,       /* u.frame.count, u.frame.vals[] */
    PROTO_REC_INPUT,      /* u.input.key, u.input.pressed (host -> device) */
    PROTO_REC_UNKNOWN     /* unrecognised key or malformed value */
} proto_rec_type_t;

typedef struct {
    proto_rec_type_t type;
    union {
        struct {
            uint16_t w, h;
        } res;
        struct {
            uint8_t r, g, b;
        } colour;
        struct {
            uint8_t index;
            uint8_t rows[8];
        } tile;
        struct {
            uint16_t count;
            uint8_t vals[PROTO_MAX_FRAME_CELLS];
        } frame;
        struct {
            uint8_t key;     /* ASCII key code */
            uint8_t pressed; /* 0 = up, 1 = down, 2 = synthesise down+up */
        } input;
        struct {
            char key[16];
        } unknown;
    } u;
} proto_record_t;

typedef struct {
    char line[PROTO_MAX_LINE];
    size_t len;
    int overflowed; /* current line exceeded PROTO_MAX_LINE */
} proto_parser_t;

void proto_parser_init(proto_parser_t *p);

/* Feed bytes. If a complete line has been decoded, fills *rec and
 * returns the number of bytes consumed (>= 1). Otherwise consumes the
 * whole chunk and returns 0. Drain in a loop:
 *
 *   size_t off = 0;
 *   while (off < n) {
 *       size_t used = proto_parse(&p, data + off, n - off, &rec);
 *       if (used > 0) { handle(&rec); off += used; }
 *       else { off = n; }
 *   }
 */
size_t proto_parse(proto_parser_t *p, const uint8_t *data, size_t len,
                   proto_record_t *rec);

/* One row (8 bytes) of the ASCII-aligned ROM font for tile `ch`. */
const char *spiterm_font8x8(uint8_t ch);

/* Accessors for the large record payloads (Swift-friendly). */
uint16_t proto_frame_count(const proto_record_t *rec);
const uint8_t *proto_frame_vals(const proto_record_t *rec);
const uint8_t *proto_tile_rows(const proto_record_t *rec);

#endif /* SPITERM_PROTOCOL_H */
