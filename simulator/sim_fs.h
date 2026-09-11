/* simulator/sim_fs.h
 *
 * FatFs backend for the desktop simulator: a virtual SD card backed by
 * a real host folder (default "sdcard", created on first run and seeded
 * with the OS's boot programs). Implements the mock ff.h API from
 * tests/host/mock, so fs_core0.c runs unchanged against it.
 */
#pragma once

#include <stdbool.h>

/* Point the filesystem at `root` and create it if missing. `seed_dir`
 * (optional) supplies os.lua / editor.lua; each is copied into the root
 * when absent. Returns false when the root cannot be created/used. */
bool sim_fs_init(const char *root, const char *seed_dir);

/* The configured root (for messages). */
const char *sim_fs_root(void);
