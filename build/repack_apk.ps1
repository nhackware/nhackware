# Repacks a target APK so libnhmenu.so loads itself inside the app's own process,
# as the app's own uid. No root, no ptrace, no /dev/input, no userdebug image.
#
#   .\build\repack_apk.ps1 -Apk C:\games\base.apk -Package com.example.game
#   .\build\repack_apk.ps1 -Apk base.apk -Package com.example.game -Install
#
# Needs on PATH (or via -Sdk): apktool, javac (any JDK 11+), and the SDK
# build-tools d8 / zipalign / apksigner.
param(
    [Parameter(Mandatory = $true)][string]$Apk,
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$Abi = "x86_64",
    [string]$So,
    [string]$Sdk = $env:ANDROID_HOME,
    [string]$OutDir,
    [string]$Serial,
    [switch]$Install
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

if (-not $So) { $So = Join-Path $repo "out\android\$Abi\libnhmenu.so" }
if (-not $OutDir) { $OutDir = Join-Path $repo "out\apk" }
if (-not (Test-Path $So)) { throw "payload missing: $So  (run build_android.ps1 -Abi $Abi)" }
if (-not (Test-Path $Apk)) { throw "apk not found: $Apk" }
if (-not $Sdk) { $Sdk = $env:ANDROID_SDK_ROOT }

# ------------------------------------------------------------------ tool lookup
function Resolve-Tool([string]$name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

$apktool = Resolve-Tool "apktool"
if (-not $apktool) { $apktool = Resolve-Tool "apktool.bat" }
$javac = Resolve-Tool "javac"
if (-not $apktool) { throw "apktool not found on PATH (https://apktool.org)" }
if (-not $javac) { throw "javac not found - install a JDK 11+" }
if (-not $Sdk -or -not (Test-Path $Sdk)) { throw "SDK not found; set `$env:ANDROID_HOME or pass -Sdk" }

# newest build-tools that actually ships d8
$bt = Get-ChildItem (Join-Path $Sdk "build-tools") -Directory |
      Where-Object { Test-Path (Join-Path $_.FullName "d8.bat") } |
      Sort-Object Name -Descending | Select-Object -First 1
if (-not $bt) { throw "no build-tools with d8 under $Sdk\build-tools" }
$d8 = Join-Path $bt.FullName "d8.bat"
$zipalign = Join-Path $bt.FullName "zipalign.exe"
$apksigner = Join-Path $bt.FullName "apksigner.bat"

# compile against the platform the guest runs; 30 == Android 11
$api = 30
$androidJar = Join-Path $Sdk "platforms\android-$api\android.jar"
if (-not (Test-Path $androidJar)) {
    $androidJar = (Get-ChildItem (Join-Path $Sdk "platforms") -Directory |
                   Sort-Object Name -Descending | Select-Object -First 1).FullName + "\android.jar"
}
if (-not (Test-Path $androidJar)) { throw "no android.jar under $Sdk\platforms" }

Write-Host "[repack] apktool  $apktool"
Write-Host "[repack] build-tools $($bt.Name)"
Write-Host "[repack] android.jar $androidJar"
Write-Host "[repack] payload  $So"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$work = Join-Path $OutDir "work"
$java = Join-Path $repo "repack\java"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }

# ------------------------------------------------------------------- 1. decode
Write-Host "`n[repack] decoding APK"
& $apktool d -f --use-aapt2 -o $work $Apk
if ($LASTEXITCODE -ne 0) { throw "apktool decode failed" }

# ------------------------------------------------------ 2. drop in the payload
$libDir = Join-Path $work "lib\$Abi"
New-Item -ItemType Directory -Force -Path $libDir | Out-Null
Copy-Item $So $libDir -Force
Write-Host "[repack] staged lib\$Abi\libnhmenu.so"

# ------------------------------------------------- 3. compile the Java glue
Write-Host "[repack] compiling nh/*.java"
$classes = Join-Path $OutDir "classes"
if (Test-Path $classes) { Remove-Item $classes -Recurse -Force }
New-Item -ItemType Directory -Force -Path $classes | Out-Null

& $javac -source 8 -target 8 -bootclasspath $androidJar -d $classes `
    (Get-ChildItem "$java\nh\*.java" | ForEach-Object { $_.FullName })
if ($LASTEXITCODE -ne 0) { throw "javac failed" }

Write-Host "[repack] dexing"
$dexOut = Join-Path $OutDir "dex"
if (Test-Path $dexOut) { Remove-Item $dexOut -Recurse -Force }
New-Item -ItemType Directory -Force -Path $dexOut | Out-Null

& $d8 --release --lib $androidJar --min-api 21 --output $dexOut `
    (Get-ChildItem "$classes\nh\*.class" | ForEach-Object { $_.FullName })
if ($LASTEXITCODE -ne 0) { throw "d8 failed" }

$dex = Join-Path $dexOut "classes.dex"
if (-not (Test-Path $dex)) { throw "d8 produced no classes.dex" }

# ------------------------------------------------------- 4. patch the manifest
Write-Host "[repack] patching AndroidManifest.xml"
$manifest = Join-Path $work "AndroidManifest.xml"
$xml = Get-Content $manifest -Raw

# provider authorities must be unique per package
$authorities = "$Package.nhloader"
$provider = @"
<provider android:name="nh.NhLoader" android:authorities="$authorities" android:exported="false" android:initOrder="2147483647" />
"@

if ($xml -match 'nh\.NhLoader') {
    Write-Host "[repack] provider already present"
} elseif ($xml -match '(?s)<application\b[^>]*?/>') {
    # self-closing <application/> -> expand it
    $xml = $xml -replace '(?s)<application\b([^>]*?)/>', "`$1>$provider</application>"
    Set-Content -Path $manifest -Value $xml -NoNewline
} elseif ($xml -match '(?s)<application\b[^>]*?>') {
    $xml = $xml -replace '(?s)(<application\b[^>]*?>)', "`$1$provider"
    Set-Content -Path $manifest -Value $xml -NoNewline
} else {
    throw "could not find an <application> element in $manifest"
}
Write-Host "[repack] injected nh.NhLoader as $authorities"

# -------------------------------------------------------------------- 5. build
Write-Host "`n[repack] rebuilding APK"
$unsigned = Join-Path $OutDir "unsigned.apk"
& $apktool b --use-aapt2 $work -o $unsigned
if ($LASTEXITCODE -ne 0) { throw "apktool build failed" }

# ------------------------------------------------ 6. merge our dex in as #2
# Android loads classes.dex, classes2.dex, ... from the APK, so we do not have to
# touch the app's own dex.
Write-Host "[repack] adding classes2.dex"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$withDex = Join-Path $OutDir "with-dex.apk"
Copy-Item $unsigned $withDex -Force
$zip = [System.IO.Compression.ZipFile]::Open($withDex, 'Update')
try {
    $entry = $zip.CreateEntry("classes2.dex", [System.IO.Compression.CompressionLevel]::Optimal)
    $es = $entry.Open()
    $fs = [System.IO.File]::OpenRead($dex)
    try { $fs.CopyTo($es) } finally { $fs.Dispose(); $es.Dispose() }
} finally { $zip.Dispose() }

# --------------------------------------------------------- 7. align and sign
Write-Host "[repack] zipalign"
$aligned = Join-Path $OutDir "aligned.apk"
& $zipalign -p -f 4 $withDex $aligned
if ($LASTEXITCODE -ne 0) { throw "zipalign failed" }

$ks = Join-Path $OutDir "nh.keystore"
if (-not (Test-Path $ks)) {
    Write-Host "[repack] generating signing key"
    $keytool = Resolve-Tool "keytool"
    if (-not $keytool) { throw "keytool not found - install a JDK" }
    & $keytool -genkeypair -v -keystore $ks -alias nh -keyalg RSA -keysize 2048 `
        -validity 10000 -storepass nhmenu -keypass nhmenu `
        -dname "CN=nhackware, OU=nh, O=nh, L=none, S=none, C=US" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "keytool failed" }
}

Write-Host "[repack] signing"
$final = Join-Path $OutDir "$Package-nhmenu.apk"
& $apksigner sign --ks $ks --ks-pass pass:nhmenu --key-pass pass:nhmenu `
    --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true `
    --out $final $aligned
if ($LASTEXITCODE -ne 0) { throw "apksigner failed" }

Write-Host "`n[repack] -> $final"

# ---------------------------------------------------------------- 8. install
if ($Install) {
    $adb = if ($env:NH_ADB) { $env:NH_ADB } else { "adb" }
    $adbArgs = @()
    if ($Serial) { $adbArgs += @("-s", $Serial) }

    Write-Host "[install] the signature changed, so an existing install must go first"
    & $adb @adbArgs uninstall $Package 2>&1 | Out-Host

    & $adb @adbArgs install -r -d -g $final
    if ($LASTEXITCODE -ne 0) { throw "adb install failed" }

    Write-Host "[install] launching"
    & $adb @adbArgs shell monkey -p $Package -c android.intent.category.LAUNCHER 1 2>&1 | Out-Host
    Write-Host "[install] tail with: adb logcat -s NHMENU:V"
}
