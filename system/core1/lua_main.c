/* core1/lua_main.c
 *
 * Core 1: Lua core.
 *
 * Boots core/boot.lua (preferring core/boot.prg when present) from the SD
 * card root and runs the scheduler loop:
 * drain input events into the top program, run due timers, otherwise
 * tick(). All filesystem access goes through the RPC to core 0; core 1
 * never touches hardware.
 */

#include <stdio.h>
#include "pico/stdlib.h"

#include "program.h"
#include "system_state.h"

// Boot program loaded from the SD card root at startup (starts the
// shell, os.lua, in the foreground).
#define BOOT_SCRIPT "core/boot.lua"

void core1_entry(void)
{
    if (!program_boot(BOOT_SCRIPT, NULL)) {
        printf("Boot failed (no SD card?)\n");
    }

    for (;;) {
        atomic_fetch_add_explicit(&g_system_state.heartbeat, 1,
                                  memory_order_relaxed);
        program_scheduler_step();
    }
}
