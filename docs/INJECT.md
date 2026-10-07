# nhackware — Android 11 emulator injection

Overlay mod menu that is pushed and injected **from Windows into the emulator
guest**. The menu renders *inside* the guest, on top of the game's own frame, and
is opened by tapping the red strip pinned to the bottom edge of the screen.

```
Windows (host)                                   Emulator guest (Android 11)
-----------------                                ---------------------------
nhdeploy.exe                                     /data/local/tmp/nhmenu/
  |  adb root / setenforce 0                       nhinject          <- runs as root
  |  adb push nhinject, libnhmenu.so               libnhmenu.so
  |  adb shell nhinject <pid> libnhmenu.so  ----->   ptrace attach
  |                                                  remote mmap (raw syscall)
  |                                                  stage path
  |                                                  remote dlopen()  --> ctor
  |                                                                        |
  |                                                                        v
  |                                            hk_eglSwapBuffers <- Dobby inline hook
  |                                              ImGui draw (red bar + menu)
  |                                              /dev/input/event* (touch, EVIOCGRAB)
  |  adb logcat -s NHMENU  <--------------------- __android_log_print
```

## Layout

| path | what |
|---|---|
| `win/nhdeploy/` | Windows CLI: adb discovery, rooting, push, inject, `--watch`, `--unload` |
| `device/injector/` | `nhinject` — ptrace `.so` injector that runs **on the device** as root |
| `device/menu/` | `libnhmenu.so` — Dobby hook on `eglSwapBuffers`, ImGui overlay, evdev input |

## Build

```powershell
$env:ANDROID_NDK_HOME = "C:\Android\ndk\android-ndk-r27c"

.\build\build_android.ps1                 # x86_64 guest (AVD / LDPlayer / Nox / MuMu)
.\build\build_android.ps1 -Abi arm64-v8a  # arm64 guest

.\build\build_win.ps1                     # out\win\nhdeploy.exe
```

First configure fetches ImGui (`v1.90.9`) and Dobby via `FetchContent`; needs
network. Pin Dobby with `-DNH_DOBBY_TAG=<tag>` for reproducible builds.

Artifacts land in `out\android\<abi>\{nhinject,libnhmenu.so}` and
`out\win\nhdeploy.exe` — which is exactly where the deployer looks by default.

## Run

```powershell
cd out\win

# simplest: game already running in the emulator
.\nhdeploy.exe --package com.example.game

# launch it, inject, keep re-injecting across restarts, and tail the menu log
.\nhdeploy.exe --package com.example.game `
               --activity com.example.game/.MainActivity `
               --watch --log

# unhook without killing the process
.\nhdeploy.exe --unload --package com.example.game
```

Tapping the red bar at the bottom edge opens the menu; tapping it again closes it.

## Requirements on the guest

`nhinject` uses `ptrace`, so it needs **root inside the guest** and a permissive
SELinux. `nhdeploy` sets both up, but the image has to allow it:

- **Android Studio AVD** — use a **non-Google-Play** system image (AOSP / "Google
  APIs"), then `adb root` works and `setenforce 0` sticks.
- **LDPlayer / Nox / MuMu** — enable root in the player settings; `su` is present
  and `nhdeploy` will route through it automatically.
- **Google Play images** — no root. Injection will fail at `PTRACE_ATTACH`.

`nhdeploy` prints the guest ABI and refuses to push a mismatched payload, which
is the most common silent failure.

## Menu features

Tabs are generated from the feature table in `device/menu/src/config.cpp`. Add a
field to `MenuConfig`, add one row to `kFeatures`, done — the widget and the INI
serialisation both come from that row.

State persists to `/data/data/<pkg>/files/nhmenu.ini`, falling back to
`/data/local/tmp/nhmenu.ini`.

Feature bodies (ESP drawing, aim solve, recoil patch) are deliberately left as
the table + config only: they are entirely game-specific. Hook them in from
`nh_ui_draw()`-adjacent code or from your own `dlopen`'d module.

## Gotchas

- **`/data/local/tmp` noexec.** Some builds mount it `noexec`. `nhdeploy` probes
  for this and tells you to retry with `--dest /data/data/<pkg>/files`.
- **Linker deadlock.** `dlopen` is driven on whatever thread ptrace stopped. If
  that thread was already inside the dynamic linker, the remote call blocks. The
  2.5 s settle delay in `nhdeploy` covers process start; if you still hit it,
  retry — ptrace stops a different thread next time.
- **Touch input.** The menu reads `/dev/input/event*` directly, which needs root
  (already satisfied). It does **not** need `SYSTEM_ALERT_WINDOW`. While the menu
  is open it issues `EVIOCGRAB` so the game stops seeing your taps; turn that off
  in the `menu` tab if you need pass-through.
- **Coordinate mapping.** If taps land in the wrong place the panel is rotated or
  mirrored relative to the framebuffer — flip `swap x/y` / `invert x` / `invert y`
  in the `touch` tab.
- **GLES3 only.** The ImGui backend is initialised with `#version 300 es`. On an
  ES2-only context the overlay logs and disables itself rather than drawing
  garbage.
- **`UNLOAD`** unhooks `eglSwapBuffers` and tears down ImGui but leaves
  `libnhmenu.so` mapped — `dlclose` from a process with live threads is a crash
  factory. `nhdeploy --unload` does the real `dlclose` via the recorded handle.
