/* simulator/sim_fs.h
 *
 * FatFs backend for the desktop simulator: a virtual SD card backed by
 * a real host folder (default "sdcard", created when missing). The
 * folder is the card, so copy the Lua programs you want to run into it.
 * Implements the mock ff.h API from tests/host/mock, so fs_core0.c runs
 * unchanged against it.
 */
#pragma once

#include <stdbool.h>

/* Point the filesystem at `root`, creating the folder when it does not
 * exist. Returns false when the root cannot be created/used. */
bool sim_fs_init(const char *root);

/* The configured root (for messages). */
const char *sim_fs_root(void);

/* The configured root as an absolute path (for messages); falls back to
 * the configured text when the path cannot be resolved. */
const char *sim_fs_root_abs(void);
