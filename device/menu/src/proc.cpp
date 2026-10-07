#include "proc.h"

#include <stdio.h>
#include <string.h>

bool nh_mapped_module_path(const char *module_name, char *out, size_t out_sz) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return false;

    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char *slash = strrchr(line, '/');
        if (!slash)
            continue;
        char *nl = strchr(slash, '\n');
        if (nl)
            *nl = 0;
        if (strcmp(slash + 1, module_name) != 0)
            continue;

        char *start = strchr(line, '/');
        if (!start)
            continue;
        snprintf(out, out_sz, "%s", start);
        found = true;
        break;
    }
    fclose(f);
    return found;
}
