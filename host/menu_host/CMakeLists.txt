cmake_minimum_required(VERSION 3.22)
project(nhmenu_host CXX)

if(NOT IMGUI_DIR)
    message(FATAL_ERROR "pass -DIMGUI_DIR=<path to imgui sources>")
endif()

add_library(nhmenu_host SHARED
    dllmain.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(nhmenu_host PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(nhmenu_host PRIVATE d3d11 dxgi d3dcompiler user32 gdi32)
set_target_properties(nhmenu_host PROPERTIES CXX_STANDARD 17)
