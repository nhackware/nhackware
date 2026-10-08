cmake_minimum_required(VERSION 3.22)
project(nhmenu CXX)

if(NOT ANDROID)
  message(FATAL_ERROR "device/menu must be cross-compiled with the NDK toolchain (build/build_android.ps1)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------- ImGui
include(FetchContent)
set(NH_IMGUI_TAG "v1.90.9" CACHE STRING "ocornut/imgui tag")
# URL, not GIT_REPOSITORY: lets the build machine pull the sources with CMake's
# own downloader, so the dev box does not need git installed.
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/${NH_IMGUI_TAG}.tar.gz)
FetchContent_MakeAvailable(imgui)

add_library(nh_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)

target_include_directories(nh_imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends)

target_compile_definitions(nh_imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
target_compile_options(nh_imgui PRIVATE -Wno-unused-function)

# ---------------------------------------------------------------- Dobby
# Dobby always defines both `dobby` (SHARED) and `dobby_static` (STATIC); there is
# no DOBBY_GENERATE_SHARED switch. We want the static one folded into libnhmenu.so.
set(DOBBY_DEBUG        OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_TEST    OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # Dobby does not set this itself
set(NH_DOBBY_TAG "master" CACHE STRING "jmpews/Dobby tag (pin this for reproducible builds)")
FetchContent_Declare(dobby
  URL https://github.com/jmpews/Dobby/archive/refs/heads/${NH_DOBBY_TAG}.tar.gz)
FetchContent_MakeAvailable(dobby)

# ---------------------------------------------------------------- menu
add_library(nhmenu SHARED
  src/entry.cpp
  src/overlay.cpp
  src/ui_menu.cpp
  src/input.cpp
  src/jni_input.cpp
  src/config.cpp
  src/proc.cpp)

target_include_directories(nhmenu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nhmenu PRIVATE nh_imgui dobby_static EGL GLESv3 log android)

# Keep the constructor; -ffunction-sections plus a version script would strip it.
target_link_options(nhmenu PRIVATE -Wl,--build-id=none)
set_target_properties(nhmenu PROPERTIES OUTPUT_NAME "nhmenu")

# JNI_OnLoad / Java_nh_NhTouch_onMotion carry JNIEXPORT (default visibility), so
# -fvisibility=hidden below does not hide them.
target_compile_options(nhmenu PRIVATE -fvisibility=hidden -fno-omit-frame-pointer)
