// nhext - external reader for the game process. Runs OUTSIDE the game, as root,
// so the anti-cheat (which lives inside the game) never sees an injected module,
// a modified APK, or a hook. It only ever sees a clean, stock process.
//
//   su -c /data/local/tmp/nhext com.axlebolt.standoff2
//
// Milestone 1: locate the pid and prove cross-process reads via /proc/<pid>/.
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
        if (fread(cmd, 1, sizeof(cmd) - 1, f) == 0) {
        }
        fclose(f);
        if (strcmp(cmd, pkg) == 0) {
            pid_t pid = (pid_t)atoi(e->d_name);
            closedir(d);
            return pid;
        }
    }
    closedir(d);
    return -1;

    (void)pkg;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <package>\n", argv[0]);
        return 2;
    }

    pid_t pid = find_pid(argv[1]);
    if (pid < 0) {
        fprintf(stderr, "[nhext] process '%s' not found\n", argv[1]);
        return 1;
    }
    printf("[nhext] pid %d\n", pid);

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
