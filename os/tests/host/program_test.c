/* program_test.c
 *
 * Host-side tests for the process model (program.c + sys_lua.c):
 * launch/pause/resume with video state restore, tick-error termination,
 * timers (incl. pause/resume deadline shifting), input event deposit,
 * and failed launches.
 *
 * The RPC wait hook runs the core 0 service inline, exactly as the
 * firmware main loop would; test programs live in the mock SD card
 * (mock_set_file) and record their activity in boot.log.
 *
 * Build and run:
 *   cmake -S tests/host -B build-host
 *   cmake --build build-host
 *   ./build-host/spicomputer_program_tests
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "fs_core0.h"
#include "program.h"
#include "rpc.h"
#include "system_state.h"

extern void mock_set_file(const char *path, const char *content);
extern void mock_clear_boot_log(void);
extern const char *mock_boot_log(void);

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

static void expect_log(const char *expected) {
    const char *got = mock_boot_log();
    if (strcmp(got, expected) != 0) {
        printf("FAIL: boot.log mismatch\n  expected: %s  got:      %s\n",
               expected, got);
        g_failures++;
    }
}

static void boot(const char *path) {
    mock_clear_boot_log();
    if (!program_boot(path, NULL)) {
        printf("FAIL: boot %s\n", path);
        g_failures++;
    }
}

/* ---------------- test 1: launch, resume, video restore ---------------- */

static const char *A_LUA =
    "local function log(m) local f = fs.open('boot.log','a') f:write(m) f:close() end\n"
    "local ticks = 0\n"
    "function setup() log('A-setup\\n') end\n"
    "function tick()\n"
    "  ticks = ticks + 1\n"
    "  log('A-tick' .. ticks .. '\\n')\n"
    "  if ticks == 2 then\n"
    "    local ok, err = Launch('b.lua')\n"
    "    if not ok then log('launch-fail:' .. tostring(err) .. '\\n') end\n"
    "  end\n"
    "  if ticks == 3 then log('A-resumed\\n') ExitProgram() end\n"
    "end\n"
    "function finish() log('A-finish\\n') end\n";

static const char *B_LUA =
    "local function log(m) local f = fs.open('boot.log','a') f:write(m) f:close() end\n"
    "function setup() log('B-setup\\n') end\n"
    "function tick() log('B-tick1\\n') ExitProgram() end\n"
    "function finish() log('B-finish\\n') end\n";

static void test_launch_resume_video(void) {
    mock_set_file("a.lua", A_LUA);
    mock_set_file("b.lua", B_LUA);
    boot("a.lua");

    program_t *a = program_top();
    CHECK(a != NULL && a->pid == 0, "a.lua is pid 0");
    CHECK(g_current_video == a->video, "a.lua video current");

    /* Simulate video content in A's state. */
    a->video->mode = 1;
    a->video->char_map[0] = 42;

    /* Step 1: A tick 1. */
    program_scheduler_step();
    CHECK(strcmp(mock_boot_log(), "A-setup\nA-tick1\n") == 0, "A tick 1");

    /* Step 2: A tick 2 -> launches B (B setup runs inside Launch). */
    program_scheduler_step();
    CHECK(strcmp(mock_boot_log(), "A-setup\nA-tick1\nA-tick2\nB-setup\n") == 0,
          "B launched and setup");
    CHECK(program_top()->pid == 1, "B is on top (pid 1)");
    CHECK(g_current_video == program_top()->video, "B video current");
    CHECK(g_current_video->mode == 0 && g_current_video->char_map[0] == 0,
          "B video state is fresh");
    CHECK(program_top()->next == a, "B stacked on A");

    /* Step 3: B tick 1 -> B exits -> A resumes. */
    program_scheduler_step();
    CHECK(program_top() == a, "A resumed after B exit");
    CHECK(g_current_video == a->video, "A video restored");
    CHECK(g_current_video->mode == 1 && g_current_video->char_map[0] == 42,
          "A video content intact");
    CHECK(strcmp(mock_boot_log(),
                 "A-setup\nA-tick1\nA-tick2\nB-setup\nB-tick1\nB-finish\n") == 0,
          "B finished cleanly");

    /* Step 4: A tick 3 -> A exits -> stack empty. */
    program_scheduler_step();
    CHECK(program_top() == NULL, "stack empty after A exit");
    CHECK(g_current_video == NULL, "no current video on empty stack");
    expect_log("A-setup\nA-tick1\nA-tick2\nB-setup\nB-tick1\nB-finish\n"
               "A-tick3\nA-resumed\nA-finish\n");
}

/* ---------------- test 2: throwing tick terminates program ---------------- */

static const char *A2_LUA =
    "local function log(m) local f = fs.open('boot.log','a') f:write(m) f:close() end\n"
    "local ticks = 0\n"
    "function setup() log('A2-setup\\n') end\n"
    "function tick()\n"
    "  ticks = ticks + 1\n"
    "  log('A2-tick' .. ticks .. '\\n')\n"
    "  if ticks == 1 then Launch('bad.lua')\n"
    "  elseif ticks == 2 then log('A2-resumed\\n') ExitProgram() end\n"
    "end\n"
    "function finish() log('A2-finish\\n') end\n";

static const char *BAD_LUA =
    "local function log(m) local f = fs.open('boot.log','a') f:write(m) f:close() end\n"
    "function setup() log('BAD-setup\\n') end\n"
    "function tick() log('BAD-tick\\n') error('boom') end\n"
    "function finish() log('BAD-finish\\n') end\n";

static void test_tick_throws(void) {
    mock_set_file("a2.lua", A2_LUA);
    mock_set_file("bad.lua", BAD_LUA);
    boot("a2.lua");

    program_scheduler_step(); /* A2 tick 1 -> launch bad */
    CHECK(program_top()->pid == 1, "bad.lua launched");
    program_scheduler_step(); /* bad tick throws -> terminated, A2 resumes */
    CHECK(program_top() != NULL && program_top()->pid == 0,
          "A2 resumed after bad crash");
    program_scheduler_step(); /* A2 tick 2 -> exit */
    CHECK(program_top() == NULL, "stack empty");
    expect_log("A2-setup\nA2-tick1\nBAD-setup\nBAD-tick\nBAD-finish\n"
               "A2-tick2\nA2-resumed\nA2-finish\n");
}

/* ---------------- test 3: timers ---------------- */

static const char *T_LUA =
    "local function log(m) local f = fs.open('boot.log','a') f:write(m) f:close() end\n"
    "local n = 0\n"
    "function setup()\n"
    "  log('T-setup\\n')\n"
    "  TimerCreate(function() log('T-oneshot\\n') end, 40, true)\n"
    "  TimerCreate(function()\n"
    "    n = n + 1\n"
    "    log('T-interval' .. n .. '\\n')\n"
    "    if n == 3 then ExitProgram() end\n"
    "  end, 30)\n"
    "end\n"
    "function tick() end\n"
    "function finish() log('T-finish\\n') end\n";

static void test_timers(void) {
    mock_set_file("t.lua", T_LUA);
    boot("t.lua");

    usleep(60 * 1000); /* both timers due */
    program_scheduler_step();
    CHECK(strcmp(mock_boot_log(), "T-setup\nT-oneshot\nT-interval1\n") == 0,
          "first timer batch");

    usleep(40 * 1000);
    program_scheduler_step();
    CHECK(strcmp(mock_boot_log(),
                 "T-setup\nT-oneshot\nT-interval1\nT-interval2\n") == 0,
          "second interval fire");

    usleep(40 * 1000);
    program_scheduler_step(); /* interval 3 -> ExitProgram */
    CHECK(program_top() == NULL, "timer exit ends program");
    expect_log("T-setup\nT-oneshot\nT-interval1\nT-interval2\nT-interval3\n"
               "T-finish\n");
}

/* ---------------- test 4: timers pause/resume shifting ---------------- */

static const char *P_LUA =
    "local function log(m) local f = fs.open('boot.log','a') f:write(m) f:close() end\n"
    "local ticks = 0\n"
    "function setup()\n"
    "  log('P-setup\\n')\n"
    "  TimerCreate(function() log('P-timer\\n') end, 100, true)\n"
    "end\n"
    "function tick()\n"
    "  ticks = ticks + 1\n"
    "  if ticks == 1 then Launch('b.lua')\n"
    "  elseif ticks == 2 then log('P-resumed\\n')\n"
    "  elseif ticks == 3 then ExitProgram() end\n"
    "end\n"
    "function finish() log('P-finish\\n') end\n";

static void test_timer_pause_resume(void) {
    mock_set_file("p.lua", P_LUA);
    mock_set_file("b.lua", B_LUA);
    boot("p.lua");

    program_scheduler_step(); /* P tick 1 -> launch B */
    CHECK(program_top()->pid == 1, "B launched");

    usleep(150 * 1000); /* P paused ~150 ms; its 100 ms timer is overdue */

    program_scheduler_step(); /* B exits -> P resumes */
    CHECK(program_top()->pid == 0, "P resumed");
    CHECK(strstr(mock_boot_log(), "P-resumed") == NULL,
          "P not ticked yet");
    CHECK(strstr(mock_boot_log(), "P-timer") == NULL,
          "no timer burst on resume (deadlines shifted)");

    program_scheduler_step(); /* P tick 2 */
    CHECK(strcmp(mock_boot_log(),
                 "P-setup\nB-setup\nB-tick1\nB-finish\nP-resumed\n") == 0,
          "P ticked after resume, no timer burst");

    usleep(120 * 1000);
    program_scheduler_step(); /* shifted timer now due */
    CHECK(strstr(mock_boot_log(), "P-timer\n") != NULL,
          "timer fires after shifted deadline");
    CHECK(strstr(mock_boot_log(), "P-resumed\nP-timer") != NULL,
          "timer fired after resume, not before");

    program_scheduler_step(); /* P tick 3 -> exit */
    CHECK(program_top() == NULL, "stack empty");
}

/* ---------------- test 5: input events deposited to program ---------------- */

static const char *I_LUA =
    "local function log(m) local f = fs.open('boot.log','a') f:write(m) f:close() end\n"
    "function setup() log('I-setup\\n') end\n"
    "function tick()\n"
    "  while true do\n"
    "    local ev = InputPoll()\n"
    "    if not ev then break end\n"
    "    log(string.format('I-ev:%s:%d:%d:%d\\n', ev.type, ev.key, ev.pressed, ev.dirs))\n"
    "  end\n"
    "  local c = InputControl(1)\n"
    "  if c then\n"
    "    log(string.format('I-joy:%d%d%d%d%d\\n',\n"
    "      c.up and 1 or 0, c.down and 1 or 0, c.left and 1 or 0,\n"
    "      c.right and 1 or 0, c.fire and 1 or 0))\n"
    "  end\n"
    "end\n";

static void push_key(uint8_t key, uint8_t pressed) {
    input_event_t ev = {0};
    ev.type = INPUT_EV_KEY;
    ev.key = key;
    ev.pressed = pressed;
    input_queue_push(&g_system_state.input, &ev);
}

static void push_control(uint8_t dirs, uint8_t pressed) {
    input_event_t ev = {0};
    ev.type = INPUT_EV_CONTROL1;
    ev.ctrl = 1;
    ev.dirs = dirs;
    ev.pressed = pressed;
    input_queue_push(&g_system_state.input, &ev);
}

static void test_input_deposit(void) {
    mock_set_file("i.lua", I_LUA);
    boot("i.lua");

    push_key(65, 1);
    push_key(65, 0);
    push_control(INPUT_DIR_UP, 1);
    push_control(INPUT_DIR_FIRE, 1);

    program_scheduler_step(); /* drain -> deposit -> tick */
    expect_log("I-setup\n"
               "I-ev:key:65:1:0\n"
               "I-ev:key:65:0:0\n"
               "I-ev:control1:0:1:1\n"
               "I-ev:control1:0:1:16\n"
               "I-joy:10001\n");

    program_terminate(program_top());
    CHECK(program_top() == NULL, "input program terminated");
}

/* ---------------- test 6: failed launch keeps parent running ---------------- */

static const char *N_LUA =
    "local function log(m) local f = fs.open('boot.log','a') f:write(m) f:close() end\n"
    "function setup() log('N-setup\\n') end\n"
    "function tick()\n"
    "  local ok, err = Launch('nope.lua')\n"
    "  if not ok and err ~= nil then log('N-launch-failed\\n') end\n"
    "  ExitProgram()\n"
    "end\n"
    "function finish() log('N-finish\\n') end\n";

static void test_failed_launch(void) {
    mock_set_file("n.lua", N_LUA);
    boot("n.lua");
    program_scheduler_step();
    CHECK(program_top() == NULL, "parent kept running and exited");
    expect_log("N-setup\nN-launch-failed\nN-finish\n");
}

/* ---------------- test 7: allocation churn does not exhaust the heap ---------------- */

/* Regression: Lua 5.5 passes a *tag* (not a size) as osize for new
 * allocations, so the capped allocator must not subtract it or the
 * per-program heap accounting underflows and every program eventually
 * dies with a bogus "not enough memory". */
static const char *C_LUA =
    "local n = 0\n"
    "function tick()\n"
    "  local t = { n, n + 1, n + 2 }\n"
    "  local j = InputControl(1)\n"
    "  local s = string.format('%d:%s', n, j and 'y' or 'n')\n"
    "  n = n + 1 + #t + #s\n"
    "end\n";

static void test_alloc_churn(void) {
    mock_set_file("c.lua", C_LUA);
    boot("c.lua");

    for (int i = 0; i < 20000; i++) {
        program_scheduler_step();
    }
    CHECK(program_top() != NULL, "program survives allocation churn");
    program_terminate(program_top());
    CHECK(program_top() == NULL, "churn program terminated");
}

int main(void) {
    setbuf(stdout, NULL);
    printf("=== process model tests ===\n");
    rpc_bind_wait(rpc_wait_host);
    rpc_bind_signal(NULL);
    if (!fs_core0_mount()) {
        printf("FAIL: mock SD mount\n");
        return 1;
    }

    test_launch_resume_video();
    test_tick_throws();
    test_timers();
    test_timer_pause_resume();
    test_input_deposit();
    test_failed_launch();
    test_alloc_churn();

    if (g_failures == 0) {
        printf("all process model tests passed\n");
        return 0;
    }
    printf("%d process model test(s) FAILED\n", g_failures);
    return 1;
}
