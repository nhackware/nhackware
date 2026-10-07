cmake_minimum_required(VERSION 3.22)
project(nhinject C)

if(NOT ANDROID)
  message(FATAL_ERROR "device/injector must be cross-compiled with the NDK toolchain (build/build_android.ps1)")
endif()

set(CMAKE_C_STANDARD 11)

add_executable(nhinject src/injector.c src/maps.c)

# PIE is mandatory for anything executable on Android 5+.
target_link_options(nhinject PRIVATE -pie -fPIE -Wl,--build-id=none)
target_compile_options(nhinject PRIVATE -fPIE)
target_link_libraries(nhinject PRIVATE dl)

set_target_properties(nhinject PROPERTIES OUTPUT_NAME "nhinject")
