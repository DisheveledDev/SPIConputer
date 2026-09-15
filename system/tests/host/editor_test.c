/* editor_test.c
 *
 * Host-side test for the editor (Phase 6): boots the REAL editor.lua
 * (read from the repo) against the mock SD, simulates keyboard input
 * through the normal event path, and verifies the rendered video state,
 * the save confirmation flow, and the file written back to the mock
 * SD card.
 *
 * Build and run:
 *   cmake -S tests/host -B build-host
 *   cmake --build build-host
 *   ./build-host/spicomputer_editor_tests
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs_core0.h"
#include "program.h"
#include "rpc.h"
#include "input.h"
#include "system_state.h"
#include "video.h"

extern void mock_set_file(const char *path, const char *content);
extern const char *mock_get_file(const char *path);

/* Like the simulator: the tests are single-threaded, so a full queue or
 * a full staging ring is drained from inside the wait. */
void video_queue_full_hook(void) {
    video_ops_drain();
}

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL: %s (line %d)\n", msg, __LINE__);                   \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void rpc_wait_host(void) {
    fs_core0_service();
}

static char *read_repo_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(buf); return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static void push_key(uint8_t key) {
    input_event_t ev = {0};
    ev.type = INPUT_EV_KEY;
    ev.key = key;
    ev.pressed = 1;
    input_queue_push(&g_system_state.input, &ev);
}

static void type(uint8_t key) {
    push_key(key);
    program_scheduler_step();
    /* Apply the editor's queued screen ops (core 0 would do this at the
     * next frame boundary). */
    video_ops_drain();
}

/* The editor's screen (core 0's active slot, mode 1, 40x30). */
static const video_state_t *ed(void) {
    return video_screen();
}

/* True when `text` appears somewhere on the overlay (dialogs are
 * centred by Overlay.Dialog, so tests look for their words rather than
 * fixed cells). */
static bool overlay_has(const char *text) {
    size_t n = strlen(text);
    for (int row = 0; row < 30; row++) {
        char line[41];
        for (int col = 0; col < 40; col++) {
            line[col] = (char)ed()->overlay_char[row * 40 + col];
        }
        line[40] = '\0';
        for (int col = 0; col + (int)n <= 40; col++) {
            if (memcmp(line + col, text, n) == 0) return true;
        }
    }
    return false;
}

static int menu_col_options(void) { return 12; }

/* Layout: row 0 menu bar, rows 1-28 text, row 29 status bar. */
#define TEXT(row, col) (ed()->base_char[((row) + 1) * 40 + (col)])
#define STATUS_ROW 29

static void test_editor(const char *editor_path) {
    char *editor_src = read_repo_file(editor_path);
    CHECK(editor_src != NULL, "editor program readable");
    mock_set_file("editor.lua", editor_src ? editor_src : "");
    mock_set_file("/data/note.txt", "hello\nworld\n");

    video_screens_init();
    CHECK(program_boot("editor.lua", "note.txt"), "editor boots with arg");
    CHECK(program_top() != NULL, "editor program running");
    video_ops_drain();
    CHECK(ed()->mode == VIDEO_MODE_TEXT40C, "editor starts in 40 columns");

    /* Menu bar on row 0, file content from row 1. */
    CHECK(ed()->base_char[1] == 'F' && ed()->base_char[4] == 'E',
          "menu bar shows FILE");
    CHECK(TEXT(0, 0) == 'h' && TEXT(0, 4) == 'o',
          "file content rendered (line 1)");
    CHECK(TEXT(1, 0) == 'w' && TEXT(1, 4) == 'd',
          "file content rendered (line 2)");
    CHECK(ed()->base_attr[40] == 0x80, "cursor is the inverted cell");
    /* Status line (row 29) inverted, starts with the file name. */
    CHECK(ed()->base_attr[STATUS_ROW * 40] == 0x80, "status line inverted");
    CHECK(ed()->base_char[STATUS_ROW * 40] == 'n' &&
              ed()->base_char[STATUS_ROW * 40 + 4] == '.',
          "status line shows file name");

    /* F4 opens OPTIONS on the overlay; Return toggles the cursor blink
     * and closes it, leaving the base layer untouched. */
    type(135);
    CHECK(ed()->base_attr[menu_col_options()] == 0x80, "F4 highlights OPTIONS");
    CHECK(overlay_has("Toggle cursor blink"), "OPTIONS menu on the overlay");
    type(13);
    CHECK(!overlay_has("Toggle cursor blink"), "menu closed after choosing");
    CHECK(TEXT(0, 0) == 'h', "text intact under the closed menu");

    type(133);
    CHECK(ed()->base_attr[1] == 0x80, "F2 highlights FILE");
    CHECK(ed()->overlay_attr[40] == 0x80 && overlay_has("Save"),
          "F2 opens the file menu window");
    type(129);
    type(13);
    CHECK(overlay_has("GO TO LINE"), "file menu opens go-to-line dialog");
    type('2');
    type(13);
    CHECK(!overlay_has("GO TO LINE"), "go-to dialog closed");
    CHECK(ed()->base_attr[2 * 40] == 0x80, "cursor moved to line 2");
    type(128);

    /* Type 'X' at the cursor (start of line 1). */
    type('X');
    CHECK(TEXT(0, 0) == 'X' && TEXT(0, 1) == 'h', "typed char inserted");

    /* Cursor right, then type 'Z'. Cursor was at col 1 (after the 'X'),
     * so 'Z' lands at col 2. */
    type(131);
    type('Z');
    CHECK(TEXT(0, 0) == 'X' && TEXT(0, 1) == 'h' && TEXT(0, 2) == 'Z' &&
              TEXT(0, 3) == 'e',
          "right + insert at cursor");

    /* Backspace deletes the 'Z'. */
    type(8);
    CHECK(TEXT(0, 0) == 'X' && TEXT(0, 1) == 'h',
          "backspace deletes before cursor");
    /* "note.txt  L1 C2  *" */
    CHECK(ed()->base_char[STATUS_ROW * 40 + 17] == '*',
          "status shows the dirty marker");

    /* Ctrl+Q with a dirty buffer -> save confirmation dialog. */
    type(17);
    CHECK(program_top() != NULL, "editor still running (confirm mode)");
    CHECK(overlay_has("UNSAVED CHANGES"), "dialog asks to save");

    /* 'y' -> save and quit. */
    type('y');
    CHECK(program_top() == NULL, "editor saved and exited");
    const char *saved = mock_get_file("/data/note.txt");
    CHECK(saved && strcmp(saved, "Xhello\nworld\n") == 0,
          "edited file saved back to SD");
    free(editor_src);
}

/* The editor is one of the Lua programs developed outside this repo, so
 * this harness needs a path to it: it is skipped when none is given (or
 * the file cannot be read). */
int main(int argc, char **argv) {
    printf("=== editor tests ===\n");
    if (argc < 2) {
        printf("skipped: pass the editor program, e.g. "
               "./spicomputer_editor_tests /path/to/editor.lua\n");
        return 0;
    }
    const char *editor_path = argv[1];
    FILE *probe = fopen(editor_path, "rb");
    if (!probe) {
        printf("skipped: cannot read '%s'\n", editor_path);
        return 0;
    }
    fclose(probe);

    rpc_bind_wait(rpc_wait_host);
    rpc_bind_signal(NULL);
    if (!fs_core0_mount()) {
        printf("FAIL: mock SD mount\n");
        return 1;
    }

    test_editor(editor_path);

    if (g_failures == 0) {
        printf("all editor tests passed\n");
        return 0;
    }
    printf("%d editor test(s) FAILED\n", g_failures);
    return 1;
}
