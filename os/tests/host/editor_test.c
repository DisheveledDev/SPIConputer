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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs_core0.h"
#include "program.h"
#include "rpc.h"
#include "system_state.h"
#include "video.h"

extern void mock_set_file(const char *path, const char *content);
extern const char *mock_get_file(const char *path);

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
}

/* The editor's video state (mode 1, 40x30). */
static const video_state_t *ed(void) {
    return program_top()->video;
}

static void test_editor(const char *editor_path) {
    char *editor_src = read_repo_file(editor_path);
    CHECK(editor_src != NULL, "editor.lua readable from repo");
    mock_set_file("editor.lua", editor_src ? editor_src : "");
    mock_set_file("note.txt", "hello\nworld\n");

    CHECK(program_boot("editor.lua", "note.txt"), "editor boots with arg");
    CHECK(program_top() != NULL, "editor program running");

    /* Initial render: first line 'hello', second 'world' on line 1. */
    CHECK(ed()->char_map[0] == 'h' && ed()->char_map[4] == 'o',
          "file content rendered (line 1)");
    CHECK(ed()->char_map[40] == 'w' && ed()->char_map[44] == 'd',
          "file content rendered (line 2)");
    /* Status line (row 29) inverted, starts with the filename. */
    CHECK(ed()->attr_map[29 * 40] == 0x80, "status line inverted");
    CHECK(ed()->char_map[29 * 40] == 'n' && ed()->char_map[29 * 40 + 3] == 'e',
          "status line shows filename");

    /* Type 'X' at the cursor (start of line 1). */
    type('X');
    CHECK(ed()->char_map[0] == 'X' && ed()->char_map[1] == 'h',
          "typed char inserted");

    /* Cursor right via an ANSI sequence (27, 91, 67), then type 'Z'.
     * Cursor was at col 1 (after the 'X'), so 'Z' lands at col 2. */
    type(27);
    type(91);
    type(67);
    type('Z');
    CHECK(ed()->char_map[0] == 'X' && ed()->char_map[1] == 'h' &&
              ed()->char_map[2] == 'Z' && ed()->char_map[3] == 'e',
          "ANSI right + insert at cursor");

    /* Backspace deletes the 'Z'. */
    type(8);
    CHECK(ed()->char_map[0] == 'X' && ed()->char_map[1] == 'h',
          "backspace deletes before cursor");

    /* Ctrl+Q with a dirty buffer -> save confirmation. */
    type(17);
    CHECK(program_top() != NULL, "editor still running (confirm mode)");
    CHECK(ed()->char_map[29 * 40] == 's', "status asks to save");

    /* 'y' -> save and quit. */
    type('y');
    CHECK(program_top() == NULL, "editor saved and exited");
    const char *saved = mock_get_file("note.txt");
    CHECK(saved && strcmp(saved, "Xhello\nworld\n") == 0,
          "edited file saved back to SD");
    free(editor_src);
}

int main(int argc, char **argv) {
    const char *editor_path = argc > 1 ? argv[1] : "../../editor.lua";
    printf("=== editor tests ===\n");
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
