#include "hook.h"
#include "log.h"

#include <elf.h>
#include <link.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <vector>

namespace nh::hook {
namespace {

struct Patched {
    void **slot;
    void *old_value;
};
std::vector<Patched> g_patched;

bool owns(void **slot) {
    for (auto &p : g_patched)
        if (p.slot == slot)
            return true;
    return false;
}

bool set_rw(void *addr, size_t len) {
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t base = (uintptr_t)addr & ~((uintptr_t)page - 1);
    size_t span = ((uintptr_t)addr + len) - base + page;
    return mprotect((void *)base, span, PROT_READ | PROT_WRITE) == 0;
}

struct Ctx {
    const char *symbol;
    void *replacement;
    int written;
};

// One loaded ELF object. dl_iterate_phdr supplies the correct load bias in
// dlpi_addr, which is exactly what the pre-bias dynamic d_ptr / r_offset need.
int visitor(struct dl_phdr_info *info, size_t, void *data) {
    Ctx *ctx = (Ctx *)data;

    // The main executable is skipped: its dynamic entries are already absolute on
    // many loaders, and on Android the eglSwapBuffers callers live in shared libs.
    if (!info->dlpi_name || info->dlpi_name[0] == '\0')
        return 0;


    const ElfW(Dyn) *dyn = nullptr;
    ElfW(Addr) min_vaddr = ~(ElfW(Addr))0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_LOAD && info->dlpi_phdr[i].p_vaddr < min_vaddr)
            min_vaddr = info->dlpi_phdr[i].p_vaddr;
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
            dyn = (const ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    }
    if (!dyn)
        return 0;
    // Some loaders pre-relocate the dynamic section (d_ptr becomes absolute); others
    // leave it pre-bias. Values at/above the mapped base are absolute, the rest need
    // the bias. Same rule applies to r_offset below.
    const ElfW(Addr) bias = info->dlpi_addr;
    const ElfW(Addr) base = bias + min_vaddr;
    auto rebase = [&](ElfW(Addr) v) { return v >= base ? v : bias + v; };

    ElfW(Sym) *symtab = nullptr;
    const char *strtab = nullptr;
    ElfW(Rela) *jmprel = nullptr;
    size_t pltrelsz = 0;
    ElfW(Rela) *rela = nullptr;
    size_t relasz = 0;

    for (const ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB:
            symtab = (ElfW(Sym) *)rebase(d->d_un.d_ptr);
            break;
        case DT_STRTAB:
            strtab = (const char *)rebase(d->d_un.d_ptr);
            break;
        case DT_JMPREL:
            jmprel = (ElfW(Rela) *)rebase(d->d_un.d_ptr);
            break;
        case DT_PLTRELSZ:
            pltrelsz = d->d_un.d_val;
            break;
        case DT_RELA:
            rela = (ElfW(Rela) *)rebase(d->d_un.d_ptr);
            break;
        case DT_RELASZ:
            relasz = d->d_un.d_val;
            break;
        }
    }
    if (!symtab || !strtab)
        return 0;

    auto scan = [&](ElfW(Rela) *r, size_t bytes) {
        size_t n = bytes / sizeof(ElfW(Rela));
        for (size_t i = 0; i < n; i++) {
            unsigned long idx = ELF64_R_SYM(r[i].r_info);
            if (strcmp(strtab + symtab[idx].st_name, ctx->symbol) != 0)
                continue;
            void **slot = (void **)rebase(r[i].r_offset);
            if (owns(slot))
                continue;
            if (!set_rw(slot, sizeof(void *)))
                continue;
            g_patched.push_back({slot, *slot});
            *slot = ctx->replacement;
            ctx->written++;
        }
    };

    if (jmprel && pltrelsz)
        scan(jmprel, pltrelsz);
    if (rela && relasz)
        scan(rela, relasz);

    return 0;
}

int patch_all(const char *symbol, void *replacement) {
    Ctx ctx{symbol, replacement, 0};
    dl_iterate_phdr(visitor, &ctx);
    return ctx.written;
}

} // namespace

bool hook_got(const char *symbol, void *replacement, void **original) {
    void *real = dlsym(RTLD_DEFAULT, symbol);
    if (!real) {
        NH_LOGW("hook: %s not resolvable", symbol);
        return false;
    }
    if (original)
        *original = real;

    int n = patch_all(symbol, replacement);
    NH_LOGI("hook: patched %d GOT slot(s) of %s", n, symbol);
    return true;
}

bool rescan(const char *symbol, void *replacement) {
    return patch_all(symbol, replacement) > 0;
}

void unhook_all() {
    size_t count = g_patched.size();
    for (auto &p : g_patched) {
        if (set_rw(p.slot, sizeof(void *)))
            *p.slot = p.old_value;
    }
    g_patched.clear();
    NH_LOGI("hook: restored %zu slot(s)", count);
}

} // namespace nh::hook
