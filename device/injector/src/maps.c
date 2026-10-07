#ifndef _GNU_SOURCE
#define _GNU_SOURCE // dladdr/Dl_info on glibc; harmless on bionic
#endif

#include "maps.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

uintptr_t nh_module_base(pid_t pid, const char *name) {
    char path[64];
    if (pid <= 0)
        snprintf(path, sizeof(path), "/proc/self/maps");
    else
        snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);

    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    uintptr_t base = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t start = 0;
        char perms[8] = {0};
        char map_path[768] = {0};
        // "start-end perms offset dev inode pathname"
        if (sscanf(line, "%lx-%*[0-9a-fA-F] %7s %*x %*s %*d %767[^\n]", &start, perms, map_path) < 2)
            continue;
        if (perms[0] == '\0' || map_path[0] != '/')
            continue;

        char *p = map_path;
        while (*p == ' ')
            p++;
        if (strcmp(basename_of(p), name) != 0)
            continue;

        base = start;
        break; // maps are sorted ascending, so the first hit is the load address
    }

    fclose(f);
    return base;
}

uintptr_t nh_remote_symbol(pid_t pid, const char *sym) {
    void *local = dlsym(RTLD_DEFAULT, sym);
    if (!local)
        return 0;

    Dl_info info;
    if (!dladdr(local, &info) || !info.dli_fbase || !info.dli_fname)
        return 0;

    const char *bn = basename_of(info.dli_fname);
    uintptr_t remote_base = nh_module_base(pid, bn);
    if (!remote_base)
        return 0;

    return remote_base + ((uintptr_t)local - (uintptr_t)info.dli_fbase);
}
