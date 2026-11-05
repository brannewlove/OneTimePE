$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $root "build"
$distDir  = Join-Path $root "PEBoot"

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
New-Item -ItemType Directory -Force -Path $distDir  | Out-Null

Write-Host "[1/3] Compiling Windows resources (icon + UAC manifest + version info)..." -ForegroundColor Cyan
& windres -I "$root\src" -c 65001 "$root\src\app.rc" -O coff -o "$buildDir\app.res"

Write-Host "[2/3] Compiling standalone Win32 x64 native executable..." -ForegroundColor Cyan
$outExe = Join-Path $distDir "OneTimePE.exe"
& clang++ -std=c++20 -O2 -Wall -Wextra `
    -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 `
    -I"$root\src" `
    "$root\src\main.cpp" "$root\src\net_sync.cpp" "$buildDir\app.res" `
    -municode -mwindows -static -s "-Wl,--gc-sections" `
    -lcomctl32 -lole32 -lshell32 -luuid -lgdi32 -luser32 -liphlpapi `
    -o "$outExe"

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
