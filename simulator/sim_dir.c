/* simulator/sim_dir.c — see sim_dir.h */
#include "sim_dir.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *sim_dir_open(const char *path) {
    return (void *)opendir(path);
}

bool sim_dir_next(void *handle, char *name, size_t name_cap) {
    DIR *d = (DIR *)handle;
    if (!d) {
        return false;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        snprintf(name, name_cap, "%s", ent->d_name);
        return true;
    }
    return false;
}

void sim_dir_close(void *handle) {
    if (handle) {
        closedir((DIR *)handle);
    }
}
