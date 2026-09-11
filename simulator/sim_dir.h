/* simulator/sim_dir.h
 *
 * Tiny directory-iteration shim. Exists because <dirent.h>'s DIR type
 * collides with the mock FatFs DIR typedef; the POSIX details stay in
 * sim_dir.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* NULL when the directory cannot be opened. */
void *sim_dir_open(const char *path);

/* Copies the next entry name (directories included, "." skipped).
 * Returns false when the directory is exhausted. */
bool sim_dir_next(void *handle, char *name, size_t name_cap);

void sim_dir_close(void *handle);
