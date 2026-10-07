#pragma once

#include <stddef.h>

// Absolute path of an already-mapped module, read out of /proc/self/maps.
bool nh_mapped_module_path(const char *module_name, char *out, size_t out_sz);
