param(
    [switch]$NoSync,   # 로컬에서 소스를 직접 수정 중일 때 원격 동기화를 건너뛰는 옵션
    [switch]$Sync      # 하위 호환성 유지 (기본값으로 자동 동기화됨)
)

$ErrorActionPreference = "Stop"
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
$OutputEncoding = [System.Text.Encoding]::UTF8
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $root "build"
$distDir = Join-Path $root "PEBoot"
$outExe = Join-Path $distDir "OneTimePE.exe"

# ==============================================================================
# 지능형 Git 자동 동기화 (타 환경에서 pull 충돌로 옛날 코드가 빌드되는 문제 영구 해결)
# ==============================================================================
if ((Test-Path (Join-Path $root ".git")) -and (Get-Command git -ErrorAction SilentlyContinue)) {
    $hasRemote = (& git -C $root remote 2>$null) -contains "origin"
    
    if ($hasRemote -and -not $NoSync) {
        # 1. 로컬에 사용자 소스코드(src/ 폴더) 수정 작업이 있는지 검사
        $srcDirty = (& git -C $root status --porcelain src/ 2>$null)
        
        if ($srcDirty) {
            Write-Host "[Git] 로컬 소스 코드(src/) 수정 작업이 감지되어 원격 동기화를 건너뛰고 로컬 소스로 빌드합니다." -ForegroundColor Yellow
        } else {
            # 2. 소스 수정이 없다면, 이전 빌드로 생성된 바이너리/캐시(PEBoot/OneTimePE.exe, build/ 등)로 인한 pull 충돌 방지
            & git -C $root checkout -- PEBoot/OneTimePE.exe 2>$null
            
            # 3. 원격 저장소 최신 커밋 확인 및 자동 갱신
            Write-Host "[Git] GitHub 원격 저장소(origin/main) 최신 소스 확인 중..." -ForegroundColor DarkCyan
            & git -C $root fetch origin 2>$null
            
            if ($LASTEXITCODE -eq 0) {
                $localCommit  = (& git -C $root rev-parse HEAD 2>$null)
                $remoteCommit = (& git -C $root rev-parse origin/main 2>$null)
                
                if ($localCommit -and $remoteCommit -and ($localCommit -ne $remoteCommit)) {
                    Write-Host "[Git] 최신 업데이트 발견! 로컬 소스를 origin/main 으로 자동 갱신합니다..." -ForegroundColor Green
                    & git -C $root reset --hard origin/main
                } else {
                    Write-Host "[Git] 로컬 소스가 이미 최신 상태(origin/main)입니다." -ForegroundColor DarkCyan
                }
            } else {
                Write-Host "[Git] 원격 연결 실패(오프라인/인증) — 현재 로컬 소스로 계속 진행합니다." -ForegroundColor DarkGray
            }
        }
    }
    
    $headInfo = (& git -C $root log -n 1 --format="%h (%ci) %s" 2>$null)
    if ($headInfo) {
        Write-Host "==================================================================" -ForegroundColor Cyan
        Write-Host "[Git 빌드 대상] $headInfo" -ForegroundColor White
        Write-Host "==================================================================" -ForegroundColor Cyan
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
