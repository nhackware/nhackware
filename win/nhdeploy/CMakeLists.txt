cmake_minimum_required(VERSION 3.22)
project(nhdeploy CXX)

if(NOT WIN32)
  message(FATAL_ERROR "win/nhdeploy is the Windows-side deployer; build it on Windows (build/build_win.ps1)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(nhdeploy src/main.cpp src/adb.cpp)

target_compile_definitions(nhdeploy PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
  UNICODE
  _UNICODE)

# Static CRT so the exe runs on a box without the VC++ redist.
if(MSVC)
  set_property(TARGET nhdeploy PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(nhdeploy PRIVATE /W3 /utf-8)
endif()
