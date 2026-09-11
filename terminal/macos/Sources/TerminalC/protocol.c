/* protocol.c — incremental parser for the serial mirror text protocol.
 * See protocol.h for the wire format. Pure C, no allocation, safe to
 * reuse on the host and (later) to reference from firmware code.
 */
#include "protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font8x8_basic.h"

void proto_parser_init(proto_parser_t *p) {
    p->len = 0;
    p->overflowed = 0;
}

/* Trim trailing CR/LF. */
static size_t trim_eol(char *line, size_t len) {
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        len--;
    }
    return len;
}

/* Split "key=value" in place: *key points at the key (NUL-terminated)
 * and *value at the value. Returns 0 on success. */
static int split_kv(char *line, size_t len, char **key, char **value) {
    size_t i;
    for (i = 0; i < len; i++) {
        if (line[i] == '=') {
            line[i] = '\0';
            *key = line;
            *value = line + i + 1;
            return 0;
        }
    }
    return -1;
}

/* Parse a decimal or 0x-hex uchar value. *cursor advances past the
 * value and any following comma. Returns 0 on success. */
static int parse_u8(char **cursor, uint8_t *out) {
    char *end;
    long v;

    while (**cursor == ' ' || **cursor == '\t') {
        (*cursor)++;
    }
    if (**cursor == '\0' || **cursor == ',') {
        return -1;
    }
    v = strtol(*cursor, &end, 0);
    if (end == *cursor || v < 0 || v > 255) {
        return -1;
    }
    *out = (uint8_t)v;
    *cursor = end;
    while (**cursor == ' ' || **cursor == '\t') {
        (*cursor)++;
    }
    if (**cursor == ',') {
        (*cursor)++;
    }
    return 0;
}

/* Parse "WxH" into res. Returns 0 on success. */
static int parse_resolution(char *value, proto_record_t *rec) {
    char *x = strchr(value, 'x');
    char *end;
    long w, h;
    if (!x || x == value) {
        return -1;
    }
    *x = '\0';
    w = strtol(value, &end, 10);
    if (end == value || w <= 0 || w > 255) {
        return -1;
    }
    h = strtol(x + 1, &end, 10);
    if (end == x + 1 || h <= 0 || h > 255) {
        return -1;
    }
    rec->u.res.w = (uint16_t)w;
    rec->u.res.h = (uint16_t)h;
    return 0;
}

/* C64-like 16-colour palette. */
static const struct {
    const char *name;
    uint8_t r, g, b;
} k_colours[] = {
    {"black", 0, 0, 0},
    {"white", 255, 255, 255},
    {"red", 136, 0, 0},
    {"cyan", 170, 255, 238},
    {"purple", 204, 68, 204},
    {"violet", 204, 68, 204},
    {"magenta", 204, 68, 204},
    {"green", 0, 204, 85},
    {"blue", 0, 0, 170},
    {"darkblue", 0, 0, 170},
    {"yellow", 238, 238, 119},
    {"orange", 221, 136, 85},
    {"brown", 102, 68, 0},
    {"lightred", 255, 119, 119},
    {"darkgrey", 51, 51, 51},
    {"darkgray", 51, 51, 51},
    {"grey", 119, 119, 119},
    {"gray", 119, 119, 119},
    {"lightgreen", 170, 255, 102},
    {"lightblue", 0, 136, 255},
    {"lightgrey", 187, 187, 187},
    {"lightgray", 187, 187, 187},
};

static int parse_colour(char *value, proto_record_t *rec) {
    size_t i;
    char name[24];
    size_t len = strlen(value);
    size_t n = len < sizeof(name) - 1 ? len : sizeof(name) - 1;

    memcpy(name, value, n);
    name[n] = '\0';
    for (i = 0; i < n; i++) {
        name[i] = (char)tolower((unsigned char)name[i]);
    }
    for (i = 0; i < sizeof(k_colours) / sizeof(k_colours[0]); i++) {
        if (strcmp(name, k_colours[i].name) == 0) {
            rec->u.colour.r = k_colours[i].r;
            rec->u.colour.g = k_colours[i].g;
            rec->u.colour.b = k_colours[i].b;
            return 0;
        }
    }
    return -1;
}

static int parse_tile(char *value, proto_record_t *rec) {
    uint8_t index;
    int i;
    if (parse_u8(&value, &index) != 0) {
        return -1;
    }
    for (i = 0; i < 8; i++) {
        if (parse_u8(&value, &rec->u.tile.rows[i]) != 0) {
            return -1;
        }
    }
    /* Extra values are tolerated; the first 8 are authoritative. */
    rec->u.tile.index = index;
    return 0;
}

static int parse_data(char *value, proto_record_t *rec) {
    uint16_t count = 0;
    while (*value != '\0' && count < PROTO_MAX_FRAME_CELLS) {
        if (parse_u8(&value, &rec->u.frame.vals[count]) != 0) {
            return -1;
        }
        count++;
    }
    rec->u.frame.count = count;
    return count > 0 ? 0 : -1;
}

/* Host -> device: input=<code> (down+up synthesised) or
 * input=<code>,1 / input=<code>,0 for explicit press/release. */
static int parse_input(char *value, proto_record_t *rec) {
    uint8_t code;
    uint8_t pressed;

    if (parse_u8(&value, &code) != 0) {
        return -1;
    }
    rec->u.input.key = code;
    if (*value == '\0') {
        rec->u.input.pressed = 2;
        return 0;
    }
    if (parse_u8(&value, &pressed) != 0 || pressed > 1) {
        return -1;
    }
    rec->u.input.pressed = pressed;
    return 0;
}

/* Decode one complete line (without trailing EOL) into *rec. */
static void decode_line(char *line, size_t len, proto_record_t *rec) {
    char *key, *value;

    len = trim_eol(line, len);
    line[len] = '\0';
    if (len == 0) {
        rec->type = PROTO_REC_UNKNOWN;
        rec->u.unknown.key[0] = '\0';
        return;
    }
    if (split_kv(line, len, &key, &value) != 0) {
        rec->type = PROTO_REC_UNKNOWN;
        snprintf(rec->u.unknown.key, sizeof(rec->u.unknown.key), "<no = >");
        return;
    }
    if (strcmp(key, "resolution") == 0) {
        rec->type = parse_resolution(value, rec) == 0 ? PROTO_REC_RESOLUTION
                                                      : PROTO_REC_UNKNOWN;
    } else if (strcmp(key, "foreground") == 0 ||
               strcmp(key, "background") == 0) {
        rec->type = parse_colour(value, rec) == 0
                        ? (key[0] == 'f' ? PROTO_REC_FOREGROUND
                                         : PROTO_REC_BACKGROUND)
                        : PROTO_REC_UNKNOWN;
    } else if (strcmp(key, "tile") == 0) {
        rec->type = parse_tile(value, rec) == 0 ? PROTO_REC_TILE
                                                : PROTO_REC_UNKNOWN;
    } else if (strcmp(key, "data") == 0) {
        rec->type =
            parse_data(value, rec) == 0 ? PROTO_REC_DATA : PROTO_REC_UNKNOWN;
    } else if (strcmp(key, "input") == 0) {
        rec->type =
            parse_input(value, rec) == 0 ? PROTO_REC_INPUT : PROTO_REC_UNKNOWN;
    } else {
        rec->type = PROTO_REC_UNKNOWN;
        snprintf(rec->u.unknown.key, sizeof(rec->u.unknown.key), "%s", key);
    }
}

size_t proto_parse(proto_parser_t *p, const uint8_t *data, size_t len,
                   proto_record_t *rec) {
    size_t i;

    for (i = 0; i < len; i++) {
        char c = (char)data[i];
        if (p->overflowed) {
            /* Discarding an oversized line; recover at the newline. */
            if (c == '\n') {
                p->overflowed = 0;
                p->len = 0;
                rec->type = PROTO_REC_UNKNOWN;
                rec->u.unknown.key[0] = '\0';
                return i + 1;
            }
            continue;
        }
        if (c == '\n') {
            decode_line(p->line, p->len, rec);
            p->len = 0;
            return i + 1;
        }
        if (p->len >= sizeof(p->line) - 1) {
            p->overflowed = 1;
            p->len = 0;
            continue;
        }
        p->line[p->len++] = c;
    }
    return 0;
}

/* The ROM font lives here so both the app and any host test that links
 * protocol.c get exactly one definition. */
const char *spiterm_font8x8(uint8_t ch) { return font8x8_basic[ch & 0x7f]; }

uint16_t proto_frame_count(const proto_record_t *rec) {
    return rec->u.frame.count;
}

const uint8_t *proto_frame_vals(const proto_record_t *rec) {
    return rec->u.frame.vals;
}

const uint8_t *proto_tile_rows(const proto_record_t *rec) {
    return rec->u.tile.rows;
}
