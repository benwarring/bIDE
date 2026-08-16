#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "files.h"
#include "wrappers.h"

#define MAX_FILES 20000
#define MAX_DEPTH 24

/* A hardcoded skip list rather than real .gitignore parsing. The gitignore spec
 * has negation, directory-only patterns, precedence between nested files, and
 * glob semantics that do not match fnmatch — a genuine rabbit hole. This covers
 * most of the value; real support would be its own project. */
static const char *SKIP_DIRS[] = {
    ".git", "node_modules", "build", "target", "obj", "bin", "dist",
    "vendor", ".cache", "__pycache__", ".venv", "venv", NULL
};

static char **cache = NULL;
static int cache_count = 0;
static int cache_cap = 0;
static int scanned = 0;

static int should_skip(const char *name) {
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) return 1;

    for (int i = 0; SKIP_DIRS[i]; i++) {
        if (strcmp(name, SKIP_DIRS[i]) == 0) return 1;
    }
    return 0;
}

static void push(const char *path) {
    if (cache_count >= MAX_FILES) return;

    if (cache_count == cache_cap) {
        cache_cap = cache_cap ? cache_cap * 2 : 256;
        cache = Realloc(cache, sizeof(char *) * (size_t)cache_cap);
    }
    cache[cache_count++] = strdup(path);
}

static void walk(const char *dir, int depth) {
    if (depth > MAX_DEPTH) return;

    DIR *d = opendir(dir);
    if (d == NULL) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (should_skip(ent->d_name)) continue;

        char path[4096];
        int n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (n < 0 || n >= (int)sizeof(path)) continue;

        // d_type is not portable to every filesystem, so fall back to stat
        int is_dir;
        if (ent->d_type == DT_DIR) {
            is_dir = 1;
        } else if (ent->d_type == DT_UNKNOWN) {
            struct stat st;
            if (stat(path, &st) == -1) continue;
            is_dir = S_ISDIR(st.st_mode);
        } else {
            is_dir = 0;
        }

        const char *rel = path;
        if (rel[0] == '.' && rel[1] == '/') rel += 2;

        if (is_dir) walk(path, depth + 1);
        else push(rel);
    }

    closedir(d);
}

char **files_list(int *count) {
    if (!scanned) {
        walk(".", 0);
        scanned = 1;
    }
    if (count) *count = cache_count;
    return cache;
}

void files_refresh(void) {
    files_free();
    scanned = 0;
}

void files_free(void) {
    for (int i = 0; i < cache_count; i++) free(cache[i]);
    free(cache);
    cache = NULL;
    cache_count = 0;
    cache_cap = 0;
    scanned = 0;
}
