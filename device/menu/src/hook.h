#pragma once

// Dependency-free function hooking by rewriting the GOT/PLT slots of every
// loaded ELF that imports `symbol`. No trampolines, no assembly, no external
// engine - which keeps it buildable for x86_64 and arm64 alike.
namespace nh::hook {

// Resolves the true function for `symbol` and rewrites every importer's GOT slot
// to `replacement`. `*original` receives the real address so the hook can chain.
// Returns false if the symbol is not imported anywhere yet (call again later).
bool hook_got(const char *symbol, void *replacement, void **original);

// Restores every slot written by hook_got.
void unhook_all();

// Idempotent re-scan for modules that were loaded after the first hook call.
// Cheap enough to run from a timer.
bool rescan(const char *symbol, void *replacement);

} // namespace nh::hook
