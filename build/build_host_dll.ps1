# build/build_host_dll.ps1 - build the injectable Windows ImGui DLL (nhmenu_host.dll).
# Downloads imgui, picks a Windows compiler (MinGW-w64 first, then MSVC), builds.
#
#   powershell -ExecutionPolicy Bypass -File build\build_host_dll.ps1
param([string]$ImguiVer = "1.90.9")

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

$imgui = Join-Path $repo "out\deps\imgui"
if (-not (Test-Path (Join-Path $imgui "imgui.cpp"))) {
    Write-Host "[host] downloading imgui v$ImguiVer"
    $zip = Join-Path $env:TEMP "imgui.zip"
    Invoke-WebRequest "https://codeload.github.com/ocornut/imgui/zip/refs/tags/v$ImguiVer" -OutFile $zip
    $tmp = Join-Path $env:TEMP "imgui-x"
    Expand-Archive $zip $tmp -Force
    New-Item -ItemType Directory -Path (Split-Path $imgui) -Force | Out-Null
    Move-Item (Join-Path $tmp "imgui-$ImguiVer") $imgui -Force
}

# pick a compiler
$cc = $cxx = $null
if (Get-Command "x86_64-w64-mingw32-gcc" -ErrorAction SilentlyContinue) {
    $cc = "x86_64-w64-mingw32-gcc"; $cxx = "x86_64-w64-mingw32-g++"
} elseif (Get-Command "gcc" -ErrorAction SilentlyContinue) {
    $cc = "gcc"; $cxx = "g++"
} elseif (Get-Command "cl" -ErrorAction SilentlyContinue) {
    $cc = $null  # MSVC default
} else {
    Write-Host "[host] NO Windows compiler found."
    Write-Host "[host] install MSYS2 -> mingw-w64-x86_64-toolchain, add C:\msys64\mingw64\bin to PATH, then re-run."
    exit 1
}

$cfg = @("-S", (Join-Path $repo "host\menu_host"), "-B", (Join-Path $repo "out\build\menu_host"),
         "-G", "Ninja", "-DIMGUI_DIR=$imgui", "-DCMAKE_BUILD_TYPE=RelWithDebInfo")
if ($cc) { $cfg += "-DCMAKE_C_COMPILER=$cc"; $cfg += "-DCMAKE_CXX_COMPILER=$cxx" }
cmake @cfg
cmake --build (Join-Path $repo "out\build\menu_host") --parallel

$dll = Join-Path $repo "out\build\menu_host\nhmenu_host.dll"
if (Test-Path $dll) {
    Write-Host "[host] OK: $dll"
} else {
    Write-Host "[host] build failed"
    exit 1
}
