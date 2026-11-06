param(
    [switch]$Sync
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $root "build"
$distDir = Join-Path $root "PEBoot"
$outExe = Join-Path $distDir "OneTimePE.exe"

# Optional Git sync (solves git pull conflicts caused by locally rebuilt PEBoot/OneTimePE.exe or NetConfig)
if ((Test-Path (Join-Path $root ".git")) -and (Get-Command git -ErrorAction SilentlyContinue)) {
    if ($Sync) {
        Write-Host "[Git] Fetching and resetting to latest origin/main..." -ForegroundColor Yellow
        & git -C $root fetch origin
        if ($LASTEXITCODE -ne 0) { throw "git fetch origin failed." }
        & git -C $root reset --hard origin/main
        if ($LASTEXITCODE -ne 0) { throw "git reset --hard origin/main failed." }
    }
    $headInfo = (& git -C $root log -n 1 --format="%h (%ci) %s" 2>$null)
    if ($headInfo) {
        Write-Host "[Git] Building local commit: $headInfo" -ForegroundColor DarkCyan
    }
}

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
New-Item -ItemType Directory -Force -Path $distDir  | Out-Null

if (Test-Path $outExe) {
    try {
        Remove-Item -Path $outExe -Force -ErrorAction Stop
    }
    catch {
        throw "기존 OneTimePE.exe 파일을 덮어쓸 수 없습니다 ($outExe). 현재 실행 중인 OneTimePE.exe 창을 먼저 닫고 다시 빌드해 주세요."
    }
}

$cxx = $null
if (Get-Command clang++ -ErrorAction SilentlyContinue) {
    $cxx = "clang++"
}
elseif (Get-Command g++ -ErrorAction SilentlyContinue) {
    $cxx = "g++"
}
elseif (Get-Command cl -ErrorAction SilentlyContinue) {
    $cxx = "cl"
}
else {
    # Auto-detect Visual Studio MSVC if installed
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsPath) {
            $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $vcvars) {
                Write-Host "Importing Visual Studio x64 build environment..." -ForegroundColor DarkGray
                $envLines = cmd.exe /c "`"$vcvars`" >nul && set"
                foreach ($line in $envLines) {
                    if ($line -match "^([^=]+)=(.*)$") {
                        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
                    }
                }
                if (Get-Command cl -ErrorAction SilentlyContinue) {
                    $cxx = "cl"
                }
            }
        }
    }
}

if (-not $cxx) {
    throw "C++20 컴파일러(clang++, g++, 또는 Visual Studio cl.exe)를 찾을 수 없습니다."
}

Write-Host "Using compiler: $cxx" -ForegroundColor DarkGray

if ($cxx -eq "cl") {
    Write-Host "[1/3] Compiling Windows resources (MSVC rc.exe)..." -ForegroundColor Cyan
    & rc.exe /nologo /c65001 /I "$root\src" /fo "$buildDir\app.res" "$root\src\app.rc"
    if ($LASTEXITCODE -ne 0) { throw "rc.exe failed with exit code $LASTEXITCODE" }

    Write-Host "[2/3] Compiling standalone Win32 x64 native executable (MSVC cl.exe)..." -ForegroundColor Cyan
    & cl.exe /nologo /std:c++20 /O2 /EHsc /MT /utf-8 /W4 `
        /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 `
        /I "$root\src" `
        "$root\src\main.cpp" "$root\src\net_sync.cpp" "$buildDir\app.res" `
        "/Fo$buildDir\" "/Fe:$outExe" `
        /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF `
        comctl32.lib ole32.lib shell32.lib uuid.lib gdi32.lib user32.lib iphlpapi.lib ws2_32.lib
    if ($LASTEXITCODE -ne 0) { throw "cl.exe build failed with exit code $LASTEXITCODE" }
}
else {
    Write-Host "[1/3] Compiling Windows resources (icon + UAC manifest + version info)..." -ForegroundColor Cyan
    & windres -I "$root\src" -c 65001 "$root\src\app.rc" -O coff -o "$buildDir\app.res"
    if ($LASTEXITCODE -ne 0) { throw "windres failed with exit code $LASTEXITCODE" }

    Write-Host "[2/3] Compiling standalone Win32 x64 native executable ($cxx)..." -ForegroundColor Cyan
    $extraCxxArgs = @()
    if ($cxx -eq "g++") {
        # Prevent MinGW GCC from linking default-manifest.o (asInvoker) alongside our custom UAC manifest in app.res
        $specFile = Join-Path $buildDir "no_default_manifest.spec"
        Set-Content -Path $specFile -Encoding ASCII -Value "*endfile:`ncrtend.o%s`n"
        $extraCxxArgs += "-specs=$specFile"
    }

    & $cxx -std=c++20 -O2 -Wall -Wextra `
        -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 `
        -I"$root\src" `
        @extraCxxArgs `
        "$root\src\main.cpp" "$root\src\net_sync.cpp" "$buildDir\app.res" `
        -municode -mwindows -static -s "-Wl,--gc-sections" `
        -lcomctl32 -lole32 -lshell32 -luuid -lgdi32 -luser32 -liphlpapi -lws2_32 `
        -o "$outExe"
    if ($LASTEXITCODE -ne 0) { throw "$cxx build failed with exit code $LASTEXITCODE" }
}

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
$hash8 = (Get-FileHash -Path $outExe -Algorithm SHA256).Hash.Substring(0, 8)
Write-Host "`nBuild succeeded! Output: $($exeInfo.FullName) ($([Math]::Round($exeInfo.Length / 1KB, 1)) KB | Modified: $($exeInfo.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')) | SHA256: $hash8)" -ForegroundColor Green
