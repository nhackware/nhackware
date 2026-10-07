#pragma once

#include <stdint.h>
#include <sys/types.h>

// First (lowest) mapping whose basename matches `name`. Returns 0 if absent.
uintptr_t nh_module_base(pid_t pid, const char *name);

// Rebase a symbol from our own address space into the target's. The containing
// module is discovered with dladdr rather than hardcoded, so this survives linker
// layout differences (Android bionic keeps dlopen in libdl.so; glibc 2.34+ folded
// it into libc.so.6).
uintptr_t nh_remote_symbol(pid_t pid, const char *sym);
