#pragma once

// Frame callback driven from the hooked eglSwapBuffers. Everything here runs on
// the game's render thread with its EGL context already current.
namespace nh::overlay {

bool install();  // hooks eglSwapBuffers
void uninstall();
bool is_installed();

// Re-scan for modules loaded after the first hook (e.g. the game's own .so).
// Driven from the worker thread; idempotent.
void rescan_now();

// True once the UI has drawn at least one frame (menu is effectively live).
bool is_active();

} // namespace nh::overlay
