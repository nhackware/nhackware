# Convenience entry point. The two halves of nhackware are built by *different*
# toolchains and cannot share one configure:
#
#   device/  cross-compiled with the NDK   -> build/build_android.ps1
#   win/     native MSVC                   -> build/build_win.ps1
#
# This file forwards to the right subtree based on the platform it is configured
# for, so IDEs that want a single root still work.
cmake_minimum_required(VERSION 3.22)
project(nhackware NONE)

if(ANDROID)
  message(STATUS "nhackware: Android configure - building device components")
  add_subdirectory(device/injector)
  add_subdirectory(device/menu)
elseif(WIN32)
  message(STATUS "nhackware: Windows configure - building the deployer")
  add_subdirectory(win/nhdeploy)
else()
  message(FATAL_ERROR
    "nhackware has no native Linux/macOS target.\n"
    "  guest payload : build/build_android.ps1  (needs the NDK)\n"
    "  host deployer : build/build_win.ps1      (needs MSVC)")
endif()
