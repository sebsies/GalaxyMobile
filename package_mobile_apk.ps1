$ErrorActionPreference = "Stop"

$repo = Get-Location
$sdk = "C:\Users\carlo\AppData\Local\Android\Sdk"
$bt = "C:\Users\carlo\AppData\Local\Android\Sdk\build-tools\37.0.0"
$platform = "C:\Users\carlo\AppData\Local\Android\Sdk\platforms\android-37.0"
$ndk = "C:\Users\carlo\AppData\Local\Android\Sdk\ndk\28.2.13676358"
$strip = "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"
$python = "$ndk\toolchains\llvm\prebuilt\windows-x86_64\python3\python.exe"
$env:JAVA_HOME = "C:\Program Files\Java\jdk-26.0.2"
$env:PATH = "C:\Program Files\Java\jdk-26.0.2\bin;$bt;" + $env:PATH

$build = "build-mobile"
$out = "out-mobile"
$keystore = "out\debug.keystore"

if (-not (Test-Path "$out\apk\lib\arm64-v8a")) {
    New-Item -ItemType Directory -Force -Path "$out\apk\lib\arm64-v8a" | Out-Null
}

# 1. Copy and strip wrap.sh (ensure LF endings)
$wrapContent = [System.IO.File]::ReadAllText("platform\android\wrap.sh").Replace("`r`n", "`n")
[System.IO.File]::WriteAllText("$out\apk\lib\arm64-v8a\wrap.sh", $wrapContent, (New-Object System.Text.UTF8Encoding $false))

# 2. Strip and copy main game native libraries
foreach ($lib in @("libgame.so", "libmain.so", "libreserve.so")) {
    $src = "$build\$lib"
    $dst = "$out\apk\lib\arm64-v8a\$lib"
    & $strip --strip-unneeded -o $dst $src
}

# 3. Strip and copy installer native libraries
Write-Host "--- Copying installer native libraries ---"
$installerLib = "build-installer\native\libinstaller.so"
$nodtoolLib = "installer-mobile\vendor\nodtool\libnodtool.so"
& $strip --strip-unneeded -o "$out\apk\lib\arm64-v8a\libinstaller.so" $installerLib
Copy-Item $nodtoolLib "$out\apk\lib\arm64-v8a\libnodtool.so" -Force

# Copy Python runtime libraries
$pyLibDir = "installer-mobile\vendor\python\lib\arm64-v8a"
Get-ChildItem $pyLibDir -Filter "*.so" | ForEach-Object {
    Copy-Item $_.FullName "$out\apk\lib\arm64-v8a\$($_.Name)" -Force
}

# 4. Compile resources
Write-Host "--- Compiling resources with AAPT2 ---"
if (Test-Path "$out\res_platform.zip") { Remove-Item "$out\res_platform.zip" -Force }
if (Test-Path "$out\res_installer.zip") { Remove-Item "$out\res_installer.zip" -Force }
& "$bt\aapt2.exe" compile --dir platform/android/res -o "$out\res_platform.zip"
& "$bt\aapt2.exe" compile --dir installer-mobile/res -o "$out\res_installer.zip"

function Build-ApkFlavor {
    param(
        [string]$Manifest,
        [string]$OutApk,
        [string]$Label
    )

    Write-Host "--- Packaging $Label ---"
    if (Test-Path "$out\base.apk") { Remove-Item "$out\base.apk" -Force }
    if (Test-Path "$out\aligned.apk") { Remove-Item "$out\aligned.apk" -Force }

    $objDir = "$build\obj"
    $dexDir = "$build\dex"
    $rGenDir = "$build\r_gen"

    if (Test-Path $objDir) { Remove-Item -Recurse -Force $objDir }
    if (Test-Path $dexDir) { Remove-Item -Recurse -Force $dexDir }
    if (Test-Path $rGenDir) { Remove-Item -Recurse -Force $rGenDir }

    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    New-Item -ItemType Directory -Force -Path $dexDir | Out-Null
    New-Item -ItemType Directory -Force -Path $rGenDir | Out-Null

    # Link APK and generate R.java for this exact manifest
    & "$bt\aapt2.exe" link -o "$out\base.apk" --manifest $Manifest `
      -R "$out\res_platform.zip" -R "$out\res_installer.zip" --auto-add-overlay `
      --extra-packages com.galaxy.installer `
      --java $rGenDir `
      -A "installer-mobile\assets" `
      -I "$platform\android.jar" --min-sdk-version 30 --target-sdk-version 34

    # Compile Java sources against the exact R.java
    $javaFiles = @(
        "installer-mobile\src\com\galaxy\installer\CookerEngine.java",
        "installer-mobile\src\com\galaxy\installer\InstallerActivity.java",
        "installer-mobile\src\com\galaxy\installer\GalaxyFileProvider.java",
        "installer-mobile\src\com\galaxy\installer\AppInstaller.java",
        "$rGenDir\com\galaxy\installer\R.java"
    )
    & javac -source 17 -target 17 -cp "$platform\android.jar" -d $objDir $javaFiles

    $classFiles = Get-ChildItem -Recurse $objDir -Filter "*.class" | ForEach-Object { $_.FullName }
    & "$bt\d8.bat" --min-api 26 --output $dexDir $classFiles

    # Add classes.dex and native libs to base.apk
    $pyCode = @"
import zipfile, os
z = zipfile.ZipFile('$out/base.apk'.replace('\\', '/'), 'a', zipfile.ZIP_DEFLATED)
# Add classes.dex
z.write('$dexDir/classes.dex'.replace('\\', '/'), 'classes.dex')
# Add native libs
for root, _, files in os.walk('$out/apk/lib'.replace('\\', '/')):
    for f in files:
        full_p = os.path.join(root, f)
        rel_p = os.path.relpath(full_p, '$out/apk'.replace('\\', '/')).replace('\\', '/')
        z.write(full_p, rel_p)
z.close()
"@
    & $python -c $pyCode

    # 16KB page alignment
    & "$bt\zipalign.exe" -f -p 16 "$out\base.apk" "$out\aligned.apk"

    # Sign APK
    cmd.exe /c "`"$bt\apksigner.bat`" sign --ks `"$keystore`" --ks-pass pass:android --key-pass pass:android --out `"$OutApk`" `"$out\aligned.apk`""

    Remove-Item "$out\base.apk" -Force
    Remove-Item "$out\aligned.apk" -Force

    $apkInfo = Get-Item $OutApk
    Write-Host "Successfully packaged ${Label}: $OutApk ($($apkInfo.Length) bytes)"
}

# 1. Build GalaxyMobile+ first (so installer-mobile\assets doesn't bundle itself)
Build-ApkFlavor -Manifest "platform\mobile\AndroidManifest_plus.xml" -OutApk "$out\GalaxyMobilePlus.apk" -Label "GalaxyMobile+ (Mods Enabled)"

# 2. Copy GalaxyMobilePlus.apk into installer-mobile\assets\ so GalaxyMobile bundles it
Copy-Item "$out\GalaxyMobilePlus.apk" "installer-mobile\assets\GalaxyMobilePlus.apk" -Force

# 3. Build GalaxyMobile (Vanilla) with bundled GalaxyMobilePlus.apk
Build-ApkFlavor -Manifest "platform\mobile\AndroidManifest.xml" -OutApk "$out\GalaxyMobile.apk" -Label "GalaxyMobile (Vanilla)"

# 4. Clean up temporary bundled APK in installer assets
if (Test-Path "installer-mobile\assets\GalaxyMobilePlus.apk") {
    Remove-Item "installer-mobile\assets\GalaxyMobilePlus.apk" -Force
}

if (Test-Path "$out\res_platform.zip") { Remove-Item "$out\res_platform.zip" -Force }
if (Test-Path "$out\res_installer.zip") { Remove-Item "$out\res_installer.zip" -Force }
Write-Host "=== Both APKs built and signed successfully! ==="
