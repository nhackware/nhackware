#pragma once

struct ImGuiIO;

namespace nh::input {

// Opens /dev/input/event* and reads touch events directly. Works because the
// library was injected from a root shell; no SYSTEM_ALERT_WINDOW needed.
void start();
void stop();

// Called once per rendered frame from the render thread.
void update(ImGuiIO &io, float display_w, float display_h);

// Exclusive grab: while grabbed, the game stops receiving touch. Used to keep
// the game from reacting to taps that land on the menu. Root-only (EVIOCGRAB);
// a no-op on the JNI backend, which mirrors events instead of stealing them.
void set_grab(bool on);
bool is_grabbed();

// True when the JNI/MotionEvent backend is in use, i.e. the no-root build.
bool using_jni();

// Diagnostic for the menu's touch tab.
const char *device_name();
bool is_open();

} // namespace nh::input
