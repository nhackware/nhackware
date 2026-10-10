# build/build_overlay_apk.ps1 - build the nh overlay loader APK (external menu).
# Needs: JDK (javac) + Android SDK (build-tools, platforms/android-30).
#
#   powershell -ExecutionPolicy Bypass -File build\build_overlay_apk.ps1 [-Sdk <path>]
param(
    [string]$Sdk = "",
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

if (-not $Sdk) {
    foreach ($c in @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT,
                     (Join-Path $env:LOCALAPPDATA "Android\Sdk"), "C:\Android\Sdk")) {
        if ($c -and (Test-Path $c)) { $Sdk = $c; break }
    }
}
if (-not $Sdk -or -not (Test-Path $Sdk)) {
    Write-Host "[apk] SDK not found. pass -Sdk <path-to-android-sdk>"; exit 1
}
$bt = (Get-ChildItem (Join-Path $Sdk "build-tools") -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
$jar = Join-Path $Sdk "platforms\android-30\android.jar"
if (-not (Test-Path $jar)) { $jar = (Get-ChildItem (Join-Path $Sdk "platforms") -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName + "\android.jar" }
if (-not $Out) { $Out = Join-Path $repo "out\overlay\nhoverlay.apk" }

$javac = (Get-Command javac -ErrorAction SilentlyContinue).Source
if (-not $javac -and $env:JAVA_HOME) { $javac = Join-Path $env:JAVA_HOME "bin\javac.exe" }
if (-not $javac -or -not (Test-Path $javac)) {
    Write-Host "[apk] javac not found. install a JDK (e.g. adoptium) and re-run."; exit 1
}

$work = Join-Path $repo "out\overlay"
New-Item -ItemType Directory -Force -Path $work | Out-Null
$obj = Join-Path $work "obj"
Remove-Item -Recurse -Force $obj -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $obj | Out-Null

Write-Host "[apk] javac"
& $javac -source 8 -target 8 -classpath $jar -d $obj (Get-ChildItem -Recurse (Join-Path $repo "device\overlay\src") -Filter *.java).FullName
if ($LASTEXITCODE -ne 0) { Write-Host "[apk] javac failed"; exit 1 }

Write-Host "[apk] d8"
$classes = (Get-ChildItem -Recurse $obj -Filter *.class).FullName
& "$bt\d8.bat" --output $work $classes
if ($LASTEXITCODE -ne 0) { Write-Host "[apk] d8 failed"; exit 1 }

Write-Host "[apk] aapt2 link"
$base = Join-Path $work "base.apk"
& "$bt\aapt2.exe" link -o $base -I $jar --manifest (Join-Path $repo "device\overlay\AndroidManifest.xml")
if ($LASTEXITCODE -ne 0) { Write-Host "[apk] aapt2 link failed"; exit 1 }

Write-Host "[apk] add classes.dex"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($base, "Update")
[System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, (Join-Path $work "classes.dex"), "classes.dex") | Out-Null
$zip.Dispose()

Write-Host "[apk] zipalign"
$aligned = Join-Path $work "aligned.apk"
& "$bt\zipalign.exe" -f 4 $base $aligned

Write-Host "[apk] sign"
$ks = Join-Path $work "nh.jks"
if (-not (Test-Path $ks)) {
    & keytool -genkeypair -keystore $ks -storepass android -keypass android -alias nh -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=nh"
}
& "$bt\apksigner.bat" sign --ks $ks --ks-pass pass:android --key-pass pass:android --out $Out $aligned
if ($LASTEXITCODE -ne 0) { Write-Host "[apk] sign failed"; exit 1 }

Write-Host "[apk] OK: $Out"
