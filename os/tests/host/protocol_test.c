/* protocol_test.c
 *
 * Host-side tests for the serial-mirror text protocol parser
 * (protocol.c at the OS root). Golden vectors
 * mirror the example frames in AGENTS.md.
 *
 * Build and run:
 *   cmake -S tests/host -B build-host
 *   cmake --build build-host
 *   ./build-host/spicomputer_protocol_tests
 */
#include <stdio.h>
#include <string.h>

#include "protocol.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL: %s (line %d)\n", msg, __LINE__);                   \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* Feed a buffer in 1..7-byte slices; call cb for every decoded record. */
static void feed_sliced(proto_parser_t *p, const char *text,
                        void (*cb)(const proto_record_t *, void *),
                        void *ctx) {
    size_t len = strlen(text);
    size_t off = 0;
    while (off < len) {
        size_t chunk = (len - off) % 7 + 1;
        proto_record_t rec;
        size_t used = proto_parse(p, (const uint8_t *)text + off, chunk, &rec);
        if (used > 0) {
            cb(&rec, ctx);
            off += used;
        } else {
            off += chunk;
        }
    }
}

/* ---------- test 1: example frame from AGENTS.md ---------- */

static int t1_seen[4] = {0, 0, 0, 0};
static uint8_t t1_expected[8] = {0, 0, 0, 36, 38, 41, 87, 0};

static void t1_cb(const proto_record_t *rec, void *ctx) {
    (void)ctx;
    switch (rec->type) {
    case PROTO_REC_RESOLUTION:
        CHECK(rec->u.res.w == 40 && rec->u.res.h == 30,
              "resolution must be 40x30");
        t1_seen[0]++;
        break;
    case PROTO_REC_FOREGROUND:
        CHECK(rec->u.colour.r == 238 && rec->u.colour.g == 238 &&
                  rec->u.colour.b == 119,
              "foreground=yellow must map to C64 yellow");
        t1_seen[1]++;
        break;
    case PROTO_REC_BACKGROUND:
        CHECK(rec->u.colour.r == 0 && rec->u.colour.g == 0 &&
                  rec->u.colour.b == 170,
              "background=darkblue must map to C64 blue");
        t1_seen[2]++;
        break;
    case PROTO_REC_DATA: {
        size_t i;
        CHECK(rec->u.frame.count == 1200, "data frame must hold 1200 cells");
        for (i = 0; i < 8; i++) {
            CHECK(rec->u.frame.vals[i] == t1_expected[i],
                  "first 8 tile values must match the example");
        }
        /* Spot-check generated tail values. */
        CHECK(rec->u.frame.vals[1199] == (uint8_t)((1199 * 7 + 3) % 256),
              "tail value must round-trip");
        t1_seen[3]++;
        break;
    }
    default:
        CHECK(0, "unexpected record type in example frame");
    }
}

static void test_example_frame(void) {
    static char frame[PROTO_MAX_LINE];
    size_t i, n;
    proto_parser_t p;

    proto_parser_init(&p);
    memcpy(frame,
           "resolution=40x30\n"
           "foreground=yellow\n"
           "background=darkblue\n"
           "data=",
           strlen("resolution=40x30\n"
                  "foreground=yellow\n"
                  "background=darkblue\n"
                  "data="));
    n = strlen(frame);
    for (i = 0; i < 1200; i++) {
        uint8_t v = i < 8 ? t1_expected[i] : (uint8_t)((i * 7 + 3) % 256);
        n += (size_t)snprintf(frame + n, sizeof(frame) - n,
                              i == 0 ? "%u" : ",%u", v);
    }
    frame[n++] = '\n';

    feed_sliced(&p, frame, t1_cb, NULL);

    CHECK(t1_seen[0] == 1, "one resolution record");
    CHECK(t1_seen[1] == 1, "one foreground record");
    CHECK(t1_seen[2] == 1, "one background record");
    CHECK(t1_seen[3] == 1, "one data record");
}

/* ---------- test 2: tile line, hex values, CRLF, malformed ---------- */

static int t2_tiles = 0, t2_hex = 0, t2_unknown = 0, t2_res = 0;

static void t2_cb(const proto_record_t *rec, void *ctx) {
    (void)ctx;
    switch (rec->type) {
    case PROTO_REC_TILE: {
        int i;
        CHECK(rec->u.tile.index == 65, "tile index 65");
        for (i = 0; i < 8; i++) {
            CHECK(rec->u.tile.rows[i] == (uint8_t)i, "tile rows 0..7");
        }
        t2_tiles++;
        break;
    }
    case PROTO_REC_DATA:
        if (rec->u.frame.count == 2 && rec->u.frame.vals[0] == 65) {
            CHECK(rec->u.frame.vals[1] == 66,
                  "hex 0x41,0x42 must decode to 65,66");
            t2_hex++;
        } else if (rec->u.frame.count == 2 && rec->u.frame.vals[0] == 1) {
            CHECK(rec->u.frame.vals[1] == 2, "CRLF data 1,2");
        }
        break;
    case PROTO_REC_RESOLUTION:
        CHECK(rec->u.res.w == 80 && rec->u.res.h == 60,
              "resolution=80x60");
        t2_res++;
        break;
    case PROTO_REC_UNKNOWN:
        t2_unknown++;
        break;
    default:
        CHECK(0, "unexpected record type in test 2");
    }
}

static void test_misc_records(void) {
    proto_parser_t p;
    proto_parser_init(&p);

    feed_sliced(&p,
                "tile=65,0,1,2,3,4,5,6,7\n"
                "data=0x41,0x42\n"
                "data=1,2\r\n"
                "foo=bar\n"
                "resolution=abc\n"
                "foreground=blurple\n"
                "resolution=80x60\n",
                t2_cb, NULL);

    CHECK(t2_tiles == 1, "one tile record");
    CHECK(t2_hex == 1, "one hex data record");
    CHECK(t2_unknown == 3, "three unknown records");
    CHECK(t2_res == 1, "one valid resolution record");
}

/* ---------- test 3: split lines across feeds ---------- */

static int t3_got = 0;

static void t3_cb(const proto_record_t *rec, void *ctx) {
    (void)ctx;
    if (rec->type == PROTO_REC_RESOLUTION) {
        CHECK(rec->u.res.w == 80 && rec->u.res.h == 60,
              "split resolution record");
        t3_got++;
    }
}

static void test_split_line(void) {
    proto_parser_t p;
    proto_record_t rec;
    const char *a = "resol";
    const char *b = "ution=80x60\n";

    proto_parser_init(&p);
    CHECK(proto_parse(&p, (const uint8_t *)a, strlen(a), &rec) == 0,
          "no record until newline");
    CHECK(proto_parse(&p, (const uint8_t *)b, strlen(b), &rec) ==
              strlen(b),
          "record consumed exactly at newline");
    t3_cb(&rec, NULL);
    CHECK(t3_got == 1, "split line decoded");
}

/* ---------- test 4: oversized line recovery ---------- */

static int t4_unknown = 0, t4_after = 0;

static void t4_cb(const proto_record_t *rec, void *ctx) {
    (void)ctx;
    if (rec->type == PROTO_REC_UNKNOWN) {
        t4_unknown++;
    } else if (rec->type == PROTO_REC_DATA && rec->u.frame.count == 1) {
        CHECK(rec->u.frame.vals[0] == 9, "post-overflow data value");
        t4_after++;
    }
}

static void test_oversize_line(void) {
    proto_parser_t p;
    proto_record_t rec;
    char big[PROTO_MAX_LINE + 64];

    proto_parser_init(&p);
    memset(big, 'a', sizeof(big) - 2);
    big[sizeof(big) - 2] = '\n';
    big[sizeof(big) - 1] = '\0';

    feed_sliced(&p, big, t4_cb, NULL);
    CHECK(t4_unknown == 1, "oversized line yields one unknown record");

    /* Parser must still work afterwards. */
    CHECK(proto_parse(&p, (const uint8_t *)"data=9\n", 7, &rec) == 7,
          "parser recovers after overflow");
    t4_cb(&rec, NULL);
    CHECK(t4_after == 1, "post-overflow record decoded");
}

/* ---------- test 5: tile values as bare decimal edge cases ---------- */

static int t5_ok = 0;

static void t5_cb(const proto_record_t *rec, void *ctx) {
    (void)ctx;
    if (rec->type == PROTO_REC_DATA) {
        CHECK(rec->u.frame.count == 4, "four values");
        CHECK(rec->u.frame.vals[0] == 0 && rec->u.frame.vals[1] == 255 &&
                  rec->u.frame.vals[2] == 32 && rec->u.frame.vals[3] == 9,
              "0,255,32,9 decode");
        t5_ok++;
    }
}

static void test_value_edges(void) {
    proto_parser_t p;
    proto_parser_init(&p);
    feed_sliced(&p, "data=0,255,32,9\n", t5_cb, NULL);
    CHECK(t5_ok == 1, "edge values decoded");
}

/* ---------- test 6: host -> device input= records ---------- */

static int t6_down = 0, t6_up = 0, t6_synth = 0, t6_bad = 0;

static void t6_cb(const proto_record_t *rec, void *ctx) {
    (void)ctx;
    switch (rec->type) {
    case PROTO_REC_INPUT:
        if (rec->u.input.key != 98) {
            t6_bad++;
            break;
        }
        if (rec->u.input.pressed == 0) {
            t6_up++;
        } else if (rec->u.input.pressed == 1) {
            t6_down++;
        } else if (rec->u.input.pressed == 2) {
            t6_synth++;
        } else {
            t6_bad++;
        }
        break;
    case PROTO_REC_UNKNOWN:
        t6_bad++;
        break;
    default:
        CHECK(0, "unexpected record type in input test");
    }
}

static void test_input_records(void) {
    proto_parser_t p;
    proto_parser_init(&p);

    feed_sliced(&p,
                "input=98\n"      /* bare: synthesise down+up */
                "input=98,1\n"    /* explicit down */
                "input=98,0\n"    /* explicit up */
                "input=98,2\n"    /* invalid press flag -> unknown */
                "input=300\n"     /* out of range -> unknown */
                "input=9x\n",     /* malformed -> unknown */
                t6_cb, NULL);

    CHECK(t6_synth == 1, "bare input synthesises");
    CHECK(t6_down == 1, "explicit down parsed");
    CHECK(t6_up == 1, "explicit up parsed");
    CHECK(t6_bad == 3, "malformed input lines are unknown");
}

int main(void) {
    printf("=== protocol parser tests ===\n");
    test_example_frame();
    test_misc_records();
    test_split_line();
    test_oversize_line();
    test_value_edges();
    test_input_records();
    if (g_failures == 0) {
        printf("all protocol tests passed\n");
        return 0;
    }
    printf("%d protocol test(s) FAILED\n", g_failures);
    return 1;
}
