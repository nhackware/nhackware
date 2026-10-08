# nhackware — Android 11 emulator injection

Two ways to get `libnhmenu.so` into the game. **Route A is the one to use** — it
needs no root inside the guest, so there is nothing for a root check to find.

| | Route A — APK repack (no root) | Route B — ptrace (rooted guest) |
|---|---|---|
| how the .so loads | `System.loadLibrary` from a ContentProvider in the APK | remote `dlopen` driven over ptrace |
| uid | the app's own | root |
| `adb root` / `setenforce` | not used | required |
| touch source | Java `Window.Callback` mirror | `/dev/input` + `EVIOCGRAB` |
| root/debuggable/test-keys tells | none | several, see below |
| .so path in `/proc/self/maps` | `/data/app/.../lib/x86_64/libnhmenu.so` | `/data/local/tmp/nhmenu/libnhmenu.so` |
| cost | APK is re-signed, so Play Integrity / signature pinning breaks | none, but the guest must be rooted |

---

## Route A — repack (recommended)

The `.so` is placed in the APK's own `lib/<abi>/` and loaded by a
`ContentProvider` that Android instantiates before `Application.onCreate`. It
then runs in-process as the app's own uid: no ptrace, no root, no `/dev/input`,
stock `user` build, stock image.

```powershell
.\build\build_android.ps1 -Abi x86_64

.\build\repack_apk.ps1 -Apk C:\games\base.apk -Package com.example.game -Install

adb logcat -s NHMENU:V
```

Needs `apktool` on PATH, a JDK 11+, and SDK build-tools (`d8`, `zipalign`,
`apksigner`). The script generates a throwaway keystore under `out\apk\`.

What it does:

1. `apktool d` the APK
2. drop `libnhmenu.so` into `lib/<abi>/`
3. `javac` + `d8` the Java glue in `repack/java/nh/` and add it as `classes2.dex`
   (Android loads `classes.dex`, `classes2.dex`, … so the app's own dex is untouched)
4. inject `nh.NhLoader` into the manifest as a `<provider>` with `initOrder` maxed
5. `apktool b` → `zipalign -p 4` → `apksigner`

**The signature changes**, so an existing install must be uninstalled first (the
script does it) and anything gated on Play Integrity or signature pinning will
reject the build. For an emulator lab that is normally fine.

### Touch without root

`/dev/input/event*` is `root:input 0660`, so an unprivileged app cannot read it.
`repack/java/nh/NhTouch.java` instead wraps the resumed Activity's
`Window.Callback` and mirrors every `MotionEvent` into
`Java_nh_NhTouch_onMotion`. The game still receives the event — we copy
coordinates, we do not steal the stream, so no `EVIOCGRAB` and no root.

Activity discovery is `ActivityThread.currentActivityThread().mActivities` plus
`registerActivityLifecycleCallbacks`, because a ContentProvider runs before any
Activity exists.

`libnhmenu.so` picks the backend automatically: `nh::input::start()` tries JNI
first and only falls back to evdev when `nh/NhTouch` is absent (i.e. route B).

---

## Route B — ptrace (rooted lab guest)

```powershell
.\build\build_android.ps1
.\build\build_win.ps1
.\out\win\nhdeploy.exe --package com.example.game --watch --log
.\out\win\nhdeploy.exe --unload --package com.example.game
```

```
Windows                                       emulator guest
nhdeploy.exe                                  /data/local/tmp/nhmenu/
  adb root / setenforce 0                       nhinject      <- as root
  adb push nhinject, libnhmenu.so               libnhmenu.so
  adb shell nhinject <pid> libnhmenu.so ---->     ptrace attach
                                                  mmap RWX (single-stepped raw syscall)
                                                  remote dlopen() --> constructor
                                                                        |
                                                                        v
                                            hk_eglSwapBuffers (GOT hook (dl_iterate_phdr))
                                              ImGui -> back buffer, before the swap
                                              /dev/input/event* + EVIOCGRAB
```

`nhinject` rebases `dlopen` into the target via `dladdr` rather than assuming a
module name, so it survives linker layout differences. The remote call clears
`rax`/`orig_rax` before resuming — otherwise the kernel's syscall-restart path
rewinds `rip` by 2 and you execute `dlopen-2`.

---

## Root detection

If the game says *"disable root and come back"*, it is looking at some subset of
this. Route A trips none of it.

| tell | route A | route B on a stock AVD userdebug image |
|---|---|---|
| `ro.debuggable` | `0` | `1` |
| `ro.secure` | `1` | `0` |
| `ro.build.tags` | `release-keys` | `test-keys` |
| `ro.build.type` | `user` | `userdebug` |
| `/system/xbin/su`, `/system/bin/su` | absent | present |
| `/system` mounted `rw` | no | yes, after `adb remount` |
| Play Integrity / attestation | fails only because of the re-sign | fails hard |

If you still want route B, hide the tells rather than dropping root — `adb root`
does not need the `su` binary, so on a writable-system AVD you can:

```powershell
emulator -avd <name> -writable-system
adb root
adb remount
adb shell rm /system/xbin/su /system/bin/su          # adbd stays root without them
adb shell "sed -i 's/ro.debuggable=1/ro.debuggable=0/; s/ro.secure=0/ro.secure=1/' /system/build.prop"
adb shell "sed -i 's/test-keys/release-keys/' /system/build.prop"
adb reboot
```

`ro.*` props are latched at boot, so the edit only takes effect after a reboot,
and the emulator must come back with `-writable-system` for it to persist. On
LDPlayer/Nox the same idea applies to `/system/build.prop`, plus whatever their
own root switch leaves behind.

None of this defeats hardware attestation. Route A plus a game with no Play
Integrity gate is the reliable combination.

---

## Gotchas

- **`/data/local/tmp` noexec** (route B). `nhdeploy` probes for it and tells you
  to retry with `--dest /data/data/<pkg>/files`.
- **Linker deadlock** (route B). The remote `dlopen` runs on whichever thread
  ptrace stopped; if that thread was inside the dynamic linker the call blocks.
  `nhdeploy` waits 2.5 s after the process appears. Retry if you hit it.
- **GLES3 only.** The ImGui backend initialises with `#version 300 es`. On an
  ES2 context the overlay logs and disables itself instead of drawing garbage.
- **Coordinate mapping.** Taps landing in the wrong place means the panel is
  rotated or mirrored relative to the framebuffer: `touch` tab → `swap x/y`,
  `invert x/y`. On route A coordinates arrive in view pixels and are rescaled to
  the EGL surface automatically, so this is mostly a route-B concern.
- **`grab touch while open`** is root-only (`EVIOCGRAB`) and is a no-op on route A.
- **`UNLOAD`** unhooks `eglSwapBuffers` and tears down ImGui but leaves the `.so`
  mapped — `dlclose` with live threads is a crash factory. `nhdeploy --unload`
  does the real `dlclose` via the recorded handle.
- **Feature bodies** (ESP drawing, aim solve, recoil patch) are deliberately just
  the config table. Add a field to `MenuConfig`, add a row to `kFeatures` in
  `device/menu/src/config.cpp`, and the widget plus INI serialisation appear.
