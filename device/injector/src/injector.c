// nhinject - ptrace based .so injector for Android (x86_64 emulator / arm64 device).
//
// Runs ON the device, as root. Attaches to the target, carves RWX scratch via a
// single-stepped raw syscall, stages the library path, then calls dlopen() inside
// the target by driving its registers against a trap stub. The library's
// __attribute__((constructor)) does the rest.
//
//   nhinject <pid> <lib.so>              inject by pid
//   nhinject -n <process-name> <lib.so>  inject by process name
//   nhinject -u <pid> <handle>           dlclose a previously injected lib
//
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "maps.h"

#if defined(__x86_64__)
#define NH_X86_64 1
#include <asm/unistd.h>
struct nh_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx, rsi,
        rdi, orig_rax, rip, cs, eflags, rsp, ss, fs_base, gs_base, ds, es, fs, gs;
};
#elif defined(__aarch64__)
#define NH_ARM64 1
#include <asm-generic/unistd.h>
struct nh_regs {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};
#else
#error "nhinject supports x86_64 and arm64 only"
#endif

#define NH_SCRATCH_SIZE 0x20000                                  // 128 KiB: trap stub + stack + args
#define NH_ARG_AREA_OFF (NH_SCRATCH_SIZE - 0x1000)               // staged library path, top of the region
#define NH_STACK_TOP    (NH_SCRATCH_SIZE - 0x2000)               // remote stack grows down from here
#define NH_ARG_AREA_MAX 0x1000

static int getregs(pid_t pid, struct nh_regs *r) {
    return ptrace(PTRACE_GETREGS, pid, 0, r) == 0 ? 0 : -1;
}

static int setregs(pid_t pid, struct nh_regs *r) {
    return ptrace(PTRACE_SETREGS, pid, 0, r) == 0 ? 0 : -1;
}

static int read_mem(pid_t pid, uintptr_t addr, void *buf, size_t len) {
    uint8_t *dst = (uint8_t *)buf;
    for (size_t off = 0; off < len; off += sizeof(uint64_t)) {
        errno = 0;
        uint64_t word = (uint64_t)ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + off), 0);
        if (errno)
            return -1;
        size_t chunk = len - off < sizeof(word) ? len - off : sizeof(word);
        memcpy(dst + off, &word, chunk);
    }
    return 0;
}

static int write_mem(pid_t pid, uintptr_t addr, const void *buf, size_t len) {
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t off = 0; off < len; off += sizeof(uint64_t)) {
        uint64_t word = 0;
        size_t chunk = len - off;
        if (chunk < sizeof(word)) {
            // partial tail word: keep the bytes we are not overwriting
            if (read_mem(pid, addr + off, &word, sizeof(word)) < 0)
                return -1;
            memcpy(&word, src + off, chunk);
        } else {
            memcpy(&word, src + off, sizeof(word));
        }
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(addr + off), (void *)word) < 0)
            return -1;
    }
    return 0;
}

// Wait for the next stop, reporting the signal. Returns the stop signal, or -1 if
// the tracee died / we gave up.
static int wait_stop(pid_t pid) {
    for (int i = 0; i < 400; i++) {
        int status = 0;
        pid_t r = waitpid(pid, &status, __WALL);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "[nhinject] waitpid: %s\n", strerror(errno));
            return -1;
        }
        if (WIFSTOPPED(status)) {
            int sig = WSTOPSIG(status);
            // Any stop is interesting: SIGTRAP is what we want, SIGSEGV/SIGILL/SIGBUS
            // mean the remote call went wrong and must NOT be reported as success.
            if (sig == SIGTRAP || sig == SIGSEGV || sig == SIGILL || sig == SIGBUS ||
                sig == SIGABRT)
                return sig;
            if (ptrace(PTRACE_CONT, pid, 0, 0) < 0)
                return -1;
            continue;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            fprintf(stderr, "[nhinject] target died during remote call\n");
            return -1;
        }
    }
    fprintf(stderr, "[nhinject] timed out waiting for a stop (thread wedged in the linker?)\n");
    return -1;
}

// Execute one raw syscall in the target without needing writable scratch:
// overwrite the instruction at RIP with `syscall`/`svc #0`, single-step it, then
// restore the original bytes. Breaks the chicken-and-egg with the first mmap.
static int remote_syscall(pid_t pid, long nr, const uint64_t *args, int nargs, uint64_t *out) {
    struct nh_regs saved, cur;
    if (getregs(pid, &saved) < 0) {
        fprintf(stderr, "[nhinject] getregs: %s\n", strerror(errno));
        return -1;
    }

    uintptr_t at = 0;
    uint8_t stub[8];
    size_t stub_len;
#ifdef NH_X86_64
    at = (uintptr_t)saved.rip;
    stub[0] = 0x0F;
    stub[1] = 0x05; // syscall
    stub_len = 2;
#else
    at = (uintptr_t)saved.pc;
    uint32_t svc = 0xD4000001u; // svc #0
    memcpy(stub, &svc, 4);
    stub_len = 4;
#endif

    uint8_t orig[8];
    if (read_mem(pid, at, orig, stub_len) < 0) {
        fprintf(stderr, "[nhinject] cannot read code at 0x%lx: %s\n", (unsigned long)at, strerror(errno));
        return -1;
    }
    if (write_mem(pid, at, stub, stub_len) < 0) {
        fprintf(stderr, "[nhinject] cannot patch stub at 0x%lx: %s\n", (unsigned long)at, strerror(errno));
        return -1;
    }

    cur = saved;
#ifdef NH_X86_64
    cur.rax = (uint64_t)nr;
    uint64_t *dst[6] = {&cur.rdi, &cur.rsi, &cur.rdx, &cur.r10, &cur.r8, &cur.r9};
    for (int i = 0; i < 6; i++)
        *dst[i] = (i < nargs) ? args[i] : 0;
    cur.rip = at;
#else
    cur.regs[8] = (uint64_t)nr;
    for (int i = 0; i < 6; i++)
        cur.regs[i] = (i < nargs) ? args[i] : 0;
    cur.pc = at;
#endif

    int rc = -1;
    if (setregs(pid, &cur) == 0 && ptrace(PTRACE_SINGLESTEP, pid, 0, 0) == 0) {
        int sig = wait_stop(pid);
        struct nh_regs after;
        int gr = getregs(pid, &after);
        if (sig == SIGTRAP && gr == 0) {
            if (out) {
#ifdef NH_X86_64
                *out = after.rax;
#else
                *out = after.regs[0];
#endif
            }
            rc = 0;
        } else {
            fprintf(stderr, "[nhinject] syscall %ld stopped on signal %d, not SIGTRAP\n", nr, sig);
        }
    } else {
        fprintf(stderr, "[nhinject] singlestep setup failed: %s\n", strerror(errno));
    }

    write_mem(pid, at, orig, stub_len);
    setregs(pid, &saved);
    return rc;
}

// Execute `func(args...)` in the target, returning through a trap stub placed in
// `scratch`.
static int remote_call(pid_t pid, uintptr_t func, const uint64_t *args, int nargs,
                       uintptr_t scratch, uint64_t *out) {
    struct nh_regs saved, cur;
    if (getregs(pid, &saved) < 0) {
        fprintf(stderr, "[nhinject] getregs: %s\n", strerror(errno));
        return -1;
    }

    uintptr_t trap = scratch;
#ifdef NH_X86_64
    if (write_mem(pid, trap, "\xcc", 1) < 0)
        return -1;
#else
    {
        uint32_t brk = 0xD4200000u; // brk #0
        if (write_mem(pid, trap, &brk, sizeof(brk)) < 0)
            return -1;
    }
#endif

    cur = saved;
#ifdef NH_X86_64
    // SysV: at function entry rsp%16 == 8 (the call pushed the return address).
    uint64_t sp = (scratch + NH_STACK_TOP) & ~0xFull;
    sp -= 8;
    if (write_mem(pid, sp, &trap, sizeof(trap)) < 0)
        return -1;
    cur.rsp = sp;
    cur.rip = func;
    // The tracee was almost certainly stopped inside an interrupted syscall, so
    // rax holds -ERESTART* and orig_rax holds the syscall number. Left as-is, the
    // kernel's restart path rewinds rip by 2 on resume and we execute func-2.
    cur.rax = 0;
    cur.orig_rax = (uint64_t)-1;
    uint64_t *dst[6] = {&cur.rdi, &cur.rsi, &cur.rdx, &cur.rcx, &cur.r8, &cur.r9};
    for (int i = 0; i < 6; i++)
        *dst[i] = (i < nargs) ? args[i] : 0;
#else
    cur.sp = (scratch + NH_STACK_TOP) & ~0xFull;
    cur.pc = func;
    cur.regs[30] = trap; // lr -> brk
    cur.regs[8] = (uint64_t)-1; // syscallno: clears the arm64 syscall-restart state
    for (int i = 0; i < 8; i++)
        cur.regs[i] = (i < nargs) ? args[i] : 0;
#endif

    if (setregs(pid, &cur) < 0) {
        fprintf(stderr, "[nhinject] setregs: %s\n", strerror(errno));
        return -1;
    }
    if (ptrace(PTRACE_CONT, pid, 0, 0) < 0) {
        fprintf(stderr, "[nhinject] cont: %s\n", strerror(errno));
        return -1;
    }

    int sig = wait_stop(pid);
    struct nh_regs after;
    int gr = getregs(pid, &after);

    // Confirm the stop is our trap stub and not a fault inside the callee. On
    // x86_64 int3 leaves rip one past the stub; on arm64 brk leaves pc on it.
    uintptr_t landed = 0;
    if (gr == 0) {
#ifdef NH_X86_64
        landed = (uintptr_t)after.rip;
        if (landed == trap + 1)
            landed = trap;
#else
        landed = (uintptr_t)after.pc;
#endif
    }

    setregs(pid, &saved); // always restore, even on failure

    if (sig != SIGTRAP || gr < 0 || landed != trap) {
        fprintf(stderr,
                "[nhinject] remote call to 0x%lx stopped on signal %d at 0x%lx (trap was 0x%lx)\n",
                (unsigned long)func, sig, (unsigned long)landed, (unsigned long)trap);
        if (gr == 0) {
#ifdef NH_X86_64
            fprintf(stderr, "[nhinject]   rsp=0x%lx (stack base 0x%lx)\n",
                    (unsigned long)after.rsp, (unsigned long)scratch);
#else
            fprintf(stderr, "[nhinject]   sp=0x%lx (stack base 0x%lx)\n",
                    (unsigned long)after.sp, (unsigned long)scratch);
#endif
        }
        return -1;
    }

    if (out) {
#ifdef NH_X86_64
        *out = after.rax;
#else
        *out = after.regs[0];
#endif
    }
    return 0;
}

static pid_t pidof_name(const char *name) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "pidof %s 2>/dev/null", name);
    FILE *f = popen(cmd, "r");
    if (!f)
        return -1;
    char buf[256] = {0};
    if (!fgets(buf, sizeof(buf), f)) {
        pclose(f);
        return -1;
    }
    pclose(f);
    long p = strtol(buf, NULL, 10);
    return p > 0 ? (pid_t)p : -1;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage:\n"
            "  %s <pid> <lib.so>            inject by pid\n"
            "  %s -n <process> <lib.so>     inject by process name\n"
            "  %s -u <pid> <handle>         dlclose a previously injected lib\n",
            argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    if (getuid() != 0)
        fprintf(stderr, "[nhinject] warning: uid %d, ptrace will likely be denied\n", getuid());

    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }

    int unload = 0;
    pid_t pid = -1;
    const char *lib = NULL;
    uint64_t handle = 0;

    int ai = 1;
    if (!strcmp(argv[ai], "-u") || !strcmp(argv[ai], "--unload")) {
        unload = 1;
        ai++;
        if (argc < ai + 2) {
            usage(argv[0]);
            return 2;
        }
        pid = (pid_t)strtol(argv[ai++], NULL, 10);
        handle = strtoull(argv[ai++], NULL, 0);
    } else if (!strcmp(argv[ai], "-n")) {
        ai++;
        if (argc < ai + 2) {
            usage(argv[0]);
            return 2;
        }
        const char *pname = argv[ai++];
        pid = pidof_name(pname);
        if (pid <= 0) {
            fprintf(stderr, "[nhinject] process '%s' not running\n", pname);
            return 1;
        }
        lib = argv[ai++];
    } else {
        pid = (pid_t)strtol(argv[ai++], NULL, 10);
        lib = argv[ai++];
        if (pid <= 0) {
            fprintf(stderr, "[nhinject] bad pid\n");
            return 2;
        }
    }

    char real_lib[PATH_MAX];
    if (lib) {
        if (access(lib, R_OK) != 0) {
            fprintf(stderr, "[nhinject] cannot read %s (%s)\n", lib, strerror(errno));
            return 1;
        }
        if (!realpath(lib, real_lib))
            snprintf(real_lib, sizeof(real_lib), "%s", lib);
        lib = real_lib;
    }

    printf("[nhinject] target pid %d\n", (int)pid);

    if (ptrace(PTRACE_ATTACH, pid, 0, 0) < 0) {
        fprintf(stderr, "[nhinject] PTRACE_ATTACH failed: %s\n", strerror(errno));
        fprintf(stderr, "[nhinject]   -> need root, plus 'setenforce 0' when SELinux is enforcing\n");
        return 1;
    }

    int status = 0;
    if (waitpid(pid, &status, __WALL) < 0 || !WIFSTOPPED(status)) {
        fprintf(stderr, "[nhinject] attach wait failed: %s\n", strerror(errno));
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 1;
    }
    printf("[nhinject] attached, stopped on signal %d\n", WSTOPSIG(status));

    int rc = 1;
    uintptr_t scratch = 0;

    {
        // mmap(NULL, size, RWX, PRIVATE|ANONYMOUS, -1, 0) as a raw syscall
        uint64_t args[6] = {0, NH_SCRATCH_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)(intptr_t)-1, 0};
        uint64_t ret = 0;
        if (remote_syscall(pid, __NR_mmap, args, 6, &ret) < 0) {
            fprintf(stderr, "[nhinject] remote mmap failed\n");
            goto out;
        }
        if ((intptr_t)ret < 0 && (intptr_t)ret > -4096) {
            fprintf(stderr, "[nhinject] remote mmap errno %d\n", (int)(intptr_t)ret);
            goto out;
        }
        scratch = (uintptr_t)ret;
        printf("[nhinject] scratch @ 0x%lx\n", (unsigned long)scratch);
    }

    if (unload) {
        uintptr_t dlclose_fn = nh_remote_symbol(pid, "dlclose");
        if (!dlclose_fn) {
            fprintf(stderr, "[nhinject] cannot locate remote dlclose\n");
            goto out;
        }
        uint64_t args[1] = {handle};
        uint64_t ret = 0;
        if (remote_call(pid, dlclose_fn, args, 1, scratch, &ret) < 0) {
            fprintf(stderr, "[nhinject] remote dlclose failed\n");
            goto out;
        }
        printf("[nhinject] dlclose(0x%lx) = %ld\n", (unsigned long)handle, (long)ret);
        rc = (int)ret;
        goto out;
    }

    {
        size_t len = strlen(lib) + 1;
        if (len > NH_ARG_AREA_MAX - 16) {
            fprintf(stderr, "[nhinject] library path too long\n");
            goto out;
        }
        if (write_mem(pid, scratch + NH_ARG_AREA_OFF, lib, len) < 0) {
            fprintf(stderr, "[nhinject] failed to stage path: %s\n", strerror(errno));
            goto out;
        }
    }

    {
        uintptr_t dlopen_fn = nh_remote_symbol(pid, "dlopen");
        if (!dlopen_fn) {
            fprintf(stderr, "[nhinject] cannot locate remote dlopen (linker mismatch?)\n");
            goto out;
        }
        printf("[nhinject] remote dlopen @ 0x%lx\n", (unsigned long)dlopen_fn);

        uint64_t args[2] = {scratch + NH_ARG_AREA_OFF, RTLD_NOW};
        uint64_t h = 0;
        if (remote_call(pid, dlopen_fn, args, 2, scratch, &h) < 0) {
            fprintf(stderr, "[nhinject] remote dlopen call failed\n");
            goto out;
        }
        if (!h) {
            fprintf(stderr, "[nhinject] dlopen returned NULL (check logcat -s linker:E NHMENU:V)\n");
            goto out;
        }
        // machine-readable line the Windows deployer scrapes for --unload
        printf("NHMENU_HANDLE=0x%lx\n", (unsigned long)h);
        printf("[nhinject] injected ok\n");
        rc = 0;
    }

out:
    ptrace(PTRACE_DETACH, pid, 0, 0);
    printf("[nhinject] detached (rc=%d)\n", rc);
    return rc;
}
