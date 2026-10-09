#include "setup_dir.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static bool make_dirs(const char *dir, char *error, size_t error_size)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s", dir) >= (int)sizeof(path)) {
        snprintf(error, error_size, "path too long: %s", dir);
        return false;
    }
    for (char *p = path + 1; ; p++) {
        if (*p != '/' && *p != '\0') {
            continue;
        }
        const char saved = *p;
        *p = '\0';
        if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            snprintf(error, error_size, "cannot create %s: %s", path, strerror(errno));
            return false;
        }
        *p = saved;
        if (saved == '\0') {
            break;
        }
    }
    return true;
}

FILE *setup_template_open(const char *name)
{
    for (size_t i = 0; i < setup_template_file_count; i++) {
        if (strcmp(setup_template_files[i].name, name) == 0) {
            const char *contents = setup_template_files[i].contents;
            return fmemopen((void *)(uintptr_t)contents, strlen(contents), "r");
        }
    }
    return NULL;
}

bool setup_dir_init(const char *dir, char *error, size_t error_size)
{
    char path[PATH_MAX];
    for (size_t i = 0; i < setup_template_file_count; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, setup_template_files[i].name);
        struct stat st;
        if (stat(path, &st) == 0) {
            snprintf(error, error_size, "%s already exists, not overwriting", path);
            return false;
        }
    }

    if (snprintf(path, sizeof(path), "%s/%s", dir, SETUP_DIR_ASSETS_DIR) >= (int)sizeof(path)) {
        snprintf(error, error_size, "path too long: %s", dir);
        return false;
    }
    if (!make_dirs(path, error, error_size)) {
        return false;
    }

    for (size_t i = 0; i < setup_template_file_count; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, setup_template_files[i].name);
        FILE *f = fopen(path, "wx");
        if (f == NULL) {
            snprintf(error, error_size, "cannot create %s: %s", path, strerror(errno));
            return false;
        }
        const size_t len = strlen(setup_template_files[i].contents);
        const bool ok = fwrite(setup_template_files[i].contents, 1, len, f) == len;
        if (fclose(f) != 0 || !ok) {
            snprintf(error, error_size, "cannot write %s", path);
            return false;
        }
    }
    return true;
}
