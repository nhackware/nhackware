#pragma once

struct ImGuiIO;

// Touch source for the no-root (repacked APK) build. Reads MotionEvent
// coordinates mirrored from the Java Window.Callback wrapper, so it needs no
// root and never touches /dev/input.
namespace nh::jni_input {

// Returns true if a JavaVM was found AND nh/NhTouch is present in the process.
bool start();
void stop();
bool is_active();

void update(ImGuiIO &io, float display_w, float display_h);
const char *name();

} // namespace nh::jni_input
