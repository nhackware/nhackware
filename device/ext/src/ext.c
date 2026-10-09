// nhext - external reader for the game process. Runs OUTSIDE the game, as root,
// so the anti-cheat (which lives inside the game) never sees an injected module,
// a modified APK, or a hook. It only ever sees a clean, stock process.
//
//   su -c /data/local/tmp/nhext com.axlebolt.standoff2          # maps mode
//   su -c /data/local/tmp/nhext com.axlebolt.standoff2 page     # page-dump mode
//
// Milestone 1: locate the pid and prove cross-process reads via /proc/<pid>/.
// Milestone 2 (page mode): dump two known game pages plus their guest-physical
// addresses (pagemap). The host-side scanner uses these as anchors to locate the
// guest RAM inside the emulator process on Windows, so the final cheat needs no
// root at runtime at all.
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pid_t find_pid(const char *pkg) {
    DIR *d = opendir("/proc");
    if (!d)
        return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char cmd[256] = {0};
        fread(cmd, 1, sizeof(cmd) - 1, f);
        fclose(f);
        if (strcmp(cmd, pkg) == 0) {
            pid_t pid = (pid_t)atoi(e->d_name);
            closedir(d);
            return pid;
        }
    }
    closedir(d);
    return -1;
}

// Dump the first read-mapped page (file offset 0) of `needle` to /data/local/tmp/<out>
// and print its virtual + guest-physical address. pagemap PFNs require root.
static int dump_page(pid_t pid, const char *needle, const char *out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *m = fopen(path, "r");
    if (!m) {
        perror("[nhext] maps");
        return 1;
    }

    unsigned long long va = 0;
    char line[512];
    while (fgets(line, sizeof(line), m)) {
        unsigned long long s, e, off;
        char perms[8];
        if (sscanf(line, "%llx-%llx %7s %llx", &s, &e, perms, &off) != 4)
            continue;
        if (!strstr(line, needle))
            continue;
        if (perms[0] != 'r' || off != 0)
            continue;
        va = s;
        break;
    }
    fclose(m);
    if (!va) {
        fprintf(stderr, "[nhext] no mapped page for %s\n", needle);
        return 1;
    }

    snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
    int pm = open(path, O_RDONLY);
    if (pm < 0) {
        perror("[nhext] pagemap");
        return 1;
    }
    uint64_t entry = 0;
    ssize_t r = pread(pm, &entry, 8, (off_t)(va / 4096 * 8));
    close(pm);
    if (r != 8) {
        perror("[nhext] pagemap read");
        return 1;
    }
    if (!(entry >> 63 & 1)) {
        fprintf(stderr, "[nhext] page not present in ram\n");
        return 1;
    }
    uint64_t pa = (entry & ((1ULL << 55) - 1)) * 4096;

    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int mem = open(path, O_RDONLY);
    if (mem < 0) {
        perror("[nhext] mem");
        return 1;
    }
    static uint8_t buf[4096];
    r = pread(mem, buf, sizeof(buf), (off_t)va);
    close(mem);
    if (r != (ssize_t)sizeof(buf)) {
        perror("[nhext] mem read");
        return 1;
    }

    char outpath[128];
    snprintf(outpath, sizeof(outpath), "/data/local/tmp/%s", out);
    FILE *f = fopen(outpath, "wb");
    if (!f) {
        perror("[nhext] out");
        return 1;
    }
    fwrite(buf, 1, sizeof(buf), f);
    fclose(f);

    printf("[nhext] %s va 0x%llx pa 0x%llx -> %s\n", needle, va, pa, outpath);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <package> [page]\n", argv[0]);
        return 2;
    }

    pid_t pid = find_pid(argv[1]);
    if (pid < 0) {
        fprintf(stderr, "[nhext] process '%s' not found\n", argv[1]);
        return 1;
    }
    printf("[nhext] pid %d\n", pid);

    if (argc > 2 && strcmp(argv[2], "page") == 0) {
        int rc = 0;
        rc |= dump_page(pid, "libunity.so", "page0.bin");
        rc |= dump_page(pid, "base.apk", "page1.bin");
        return rc;
    }

    char maps[64];
    snprintf(maps, sizeof(maps), "/proc/%d/maps", pid);
    FILE *m = fopen(maps, "r");
    if (!m) {
        perror("[nhext] maps");
        return 1;
    }

    char line[512];
    unsigned long base = 0;
    while (fgets(line, sizeof(line), m)) {
        if (strstr(line, "libil2cpp.so") || strstr(line, "libunity.so") ||
            strstr(line, "base.apk")) {
            fputs(line, stdout);
            if (!base && strstr(line, "libil2cpp.so"))
                base = strtoul(line, NULL, 16);
        }
    }
    fclose(m);

    if (base)
        printf("[nhext] libil2cpp.so base 0x%lx\n", base);
    else
        printf("[nhext] libil2cpp.so not mapped (game may use base.apk dex only)\n");

    return 0;
}
