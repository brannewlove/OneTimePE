$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $root "build"
$distDir  = Join-Path $root "PEBoot"

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
New-Item -ItemType Directory -Force -Path $distDir  | Out-Null

Write-Host "[1/3] Compiling Windows resources (icon + UAC manifest + version info)..." -ForegroundColor Cyan
& windres -I "$root\src" -c 65001 "$root\src\app.rc" -O coff -o "$buildDir\app.res"
if ($LASTEXITCODE -ne 0) { throw "windres failed with exit code $LASTEXITCODE" }

Write-Host "[2/3] Compiling standalone Win32 x64 native executable..." -ForegroundColor Cyan
$outExe = Join-Path $distDir "OneTimePE.exe"
if (Test-Path $outExe) { Remove-Item -Path $outExe -Force }

$cxx = $null
$extraCxxArgs = @()
if (Get-Command clang++ -ErrorAction SilentlyContinue) {
    $cxx = "clang++"
} elseif (Get-Command g++ -ErrorAction SilentlyContinue) {
    $cxx = "g++"
    # Prevent MinGW GCC from linking default-manifest.o (asInvoker) alongside our custom UAC manifest in app.res
    $specFile = Join-Path $buildDir "no_default_manifest.spec"
    Set-Content -Path $specFile -Encoding ASCII -Value "*endfile:`ncrtend.o%s`n"
    $extraCxxArgs += "-specs=$specFile"
} else {
    throw "No C++20 compiler (clang++ or g++) found in PATH."
}
Write-Host "Using compiler: $cxx" -ForegroundColor DarkGray

& $cxx -std=c++20 -O2 -Wall -Wextra `
    -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 `
    -I"$root\src" `
    @extraCxxArgs `
    "$root\src\main.cpp" "$root\src\net_sync.cpp" "$buildDir\app.res" `
    -municode -mwindows -static -s "-Wl,--gc-sections" `
    -lcomctl32 -lole32 -lshell32 -luuid -lgdi32 -luser32 -liphlpapi `
    -o "$outExe"
if ($LASTEXITCODE -ne 0) { throw "$cxx build failed with exit code $LASTEXITCODE" }

Write-Host "[3/3] Preparing PEBoot folder with built-in boot.sdi..." -ForegroundColor Cyan
$sdiTarget = Join-Path $distDir "boot.sdi"
if (!(Test-Path $sdiTarget)) {
    $candidates = @(
        "$env:SystemRoot\Boot\DVD\EFI\boot.sdi",
        "$env:SystemRoot\Boot\DVD\PCAT\boot.sdi",
        "$env:SystemRoot\System32\Recovery\boot.sdi"
    )
    foreach ($src in $candidates) {
        if (Test-Path $src) {
            Copy-Item -Path $src -Destination $sdiTarget -Force
            Write-Host "Copied built-in boot.sdi -> $sdiTarget" -ForegroundColor Green
            break
        }
    }
}

$exeInfo = Get-Item $outExe
Write-Host "`nBuild succeeded! Output: $($exeInfo.FullName) ($([Math]::Round($exeInfo.Length / 1KB, 1)) KB)" -ForegroundColor Green
