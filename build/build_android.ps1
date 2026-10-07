# Cross-compiles nhinject + libnhmenu.so for the emulator guest ABI.
#
#   .\build\build_android.ps1                    # x86_64 (AVD / LDPlayer / Nox on Intel/AMD)
#   .\build\build_android.ps1 -Abi arm64-v8a     # arm64 guest
#
# Needs: NDK r25+ , cmake, ninja. Set $env:ANDROID_NDK_HOME or pass -Ndk.
#
# CMAKE_POLICY_VERSION_MINIMUM=3.5 is required because Dobby's CMakeLists still
# declares cmake_minimum_required(VERSION 3.5), which CMake 4.x rejects outright.
param(
    [string]$Abi = "x86_64",
    [string]$Ndk = $env:ANDROID_NDK_HOME,
    [string]$Platform = "android-30",          # Android 11
    [string]$BuildType = "RelWithDebInfo"
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Ndk) {
    foreach ($cand in @("$env:LOCALAPPDATA\Android\Sdk\ndk", "$env:ANDROID_HOME\ndk")) {
        if ($cand -and (Test-Path $cand)) {
            $Ndk = (Get-ChildItem $cand -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
            break
        }
    }
}
if (-not $Ndk -or -not (Test-Path $Ndk)) {
    throw "NDK not found. Set `$env:ANDROID_NDK_HOME or pass -Ndk <path>"
}

$toolchain = Join-Path $Ndk "build\cmake\android.toolchain.cmake"
if (-not (Test-Path $toolchain)) { throw "toolchain missing: $toolchain" }
Write-Host "[build] NDK     $Ndk"
Write-Host "[build] ABI     $Abi"
Write-Host "[build] API     $Platform"

$outDir = Join-Path $repo "out\android\$Abi"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Build-Component($name, $srcDir, $extra) {
    $b = Join-Path $repo "out\build\$name-$Abi"
    Write-Host "`n[build] === $name ==="
    & cmake -S $srcDir -B $b -G Ninja `
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" `
        -DANDROID_ABI="$Abi" `
        -DANDROID_PLATFORM="$Platform" `
        -DANDROID_STL="c++_static" `
        -DCMAKE_BUILD_TYPE="$BuildType" `
        -DCMAKE_MAKE_PROGRAM="ninja" `
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 `
        @extra
    if ($LASTEXITCODE -ne 0) { throw "configure failed for $name" }

    & cmake --build $b --parallel
    if ($LASTEXITCODE -ne 0) { throw "build failed for $name" }
}

Build-Component "nhinject" (Join-Path $repo "device\injector") @()
Build-Component "nhmenu"   (Join-Path $repo "device\menu")     @()

# Collect artifacts into out\android\<abi>\, which is where nhdeploy looks.
Copy-Item (Join-Path $repo "out\build\nhinject-$Abi\nhinject") $outDir -Force
Copy-Item (Join-Path $repo "out\build\nhmenu-$Abi\libnhmenu.so") $outDir -Force

Write-Host "`n[build] artifacts:"
Get-ChildItem $outDir | ForEach-Object { Write-Host ("  {0,-18} {1,10} bytes" -f $_.Name, $_.Length) }
Write-Host "`n[build] done -> $outDir"
