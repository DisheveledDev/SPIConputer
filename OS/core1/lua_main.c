/* core1/lua_main.c
 *
 * Core 1: Lua core.
 *
 * Runs the Lua engine and the boot script (os.lua). Later phases turn the
 * tail loop into the scheduler: drain input events -> run due timers ->
 * tick the top program on the process stack.
 *
 * Core 1 never touches hardware; all I/O is core 0's job (SD access moves
 * behind an RPC in Phase 3).
 */

#include <stdio.h>
#include "pico/stdlib.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "fatfs_lua.h"

// Boot script loaded from the SD card root at startup
#define BOOT_SCRIPT "os.lua"

void core1_entry(void)
{
    lua_State *L = luaL_newstate();
    if (L) {
        fatfs_lua_openlibs(L);

        if (fatfs_lua_mount()) {
            printf("SD card mounted, running %s...\n", BOOT_SCRIPT);
            if (fatfs_lua_run_file(L, BOOT_SCRIPT) != 0) {
                printf("Failed to run %s\n", BOOT_SCRIPT);
            }
        } else {
            printf("No SD card found. Insert a FAT32 card with %s in the root and reset.\n", BOOT_SCRIPT);
        }
        lua_close(L);
    }

    // Placeholder loop; becomes the scheduler in Phase 3/4.
    for (;;) {
        printf("Hello, world!\n");
        sleep_ms(1000);
    }
}
