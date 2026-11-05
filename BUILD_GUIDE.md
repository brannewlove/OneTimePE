# OneTimePE 프로젝트 이전(복사) 및 빌드 가이드

이 문서는 **OneTimePE** 프로젝트를 다른 PC나 개발 환경(온라인 및 오프라인/망분리 환경 포함)으로 복사하고, 소스 코드로부터 `OneTimePE.exe` 실행 파일을 직접 빌드하는 방법을 안내합니다.

---

## 0. 빌드 아키텍처 핵심 요약

* **언어 및 API**: C++20 표준, 순수 Win32 네이티브 API (외부 서드파티 라이브러리 의존성 없음)
* **타겟 아키텍처**: **Windows x64 (`amd64` / `x86_64`) 전용**
  * [`src/app.manifest`](src/app.manifest)에 `processorArchitecture="amd64"` 및 관리자 권한(`requireAdministrator`)이 선언되어 있습니다.
* **정적 링크(`-static`) 필수**:
  * 빌드된 `OneTimePE.exe`는 일반 Windows 환경의 GUI 매니저 역할뿐만 아니라, **최소한의 시스템 DLL만 존재하는 WinPE 부팅 환경 내부**에서도 무인 네트워크 설정 엔진(`OneTimePE.exe --pe-apply-net`)으로 직접 실행됩니다.
  * 따라서 런타임 DLL(`libstdc++-6.dll`, `libwinpthread-1.dll`, `vcruntime140.dll` 등)이 필요 없도록 반드시 **단일 정적 바이너리**로 빌드해야 합니다.

---

## 1. 프로젝트를 다른 환경으로 복사하기

### 방법 A. Git을 이용한 복제 (인터넷 연결 환경 권장)

타겟 PC에 Git이 설치되어 있다면 아래 명령으로 즉시 프로젝트를 복제할 수 있습니다.

```powershell
git clone https://github.com/brannewlove/OneTimePE.git
cd OneTimePE
```

> **참고**: 저장소에 이전 테스트 환경의 산출물(`build/`, `PEBoot/NetConfig/`)이 포함되어 있을 경우, 새 환경에서 빌드하기 전 아래 명령으로 정리하는 것을 권장합니다.
> ```powershell
> Remove-Item -Recurse -Force .\build, .\PEBoot\NetConfig, .\PEBoot\OneTimePE.ini -ErrorAction SilentlyContinue
> ```

---

### 방법 B. 폴더 / 압축 파일(ZIP)로 직접 복사 (오프라인·망분리 환경)

USB나 내부망 파일 공유를 통해 프로젝트를 옮길 때는 아래 기준에 맞춰 파일들을 복사합니다.

#### 복사 대상 분류

| 경로 | 필수 여부 | 설명 |
| :--- | :---: | :--- |
| `src\` (폴더 전체) | **필수** | C++20 소스(`main.cpp`, `net_sync.cpp`, `net_sync.h`) 및 리소스(`resource.h`, `app.rc`, `app.manifest`, `app.ico`) |
| `build.ps1` | **필수** | PowerShell 원클릭 빌드 스크립트 |
| `CMakeLists.txt` | 선택 (권장) | CMake 기반 빌드 설정 파일 |
| `README.md`, `BUILD_GUIDE.md` | 선택 (권장) | 사용 설명서 및 빌드 가이드 문서 |
| `build\` | 제외 가능 | 리소스 중간 컴파일 산출물(`app.res`). 빌드 시 자동 재생성됨 |
| `PEBoot\NetConfig\` | **제외 권장** | 현재 PC에서 추출된 물리 랜카드 드라이버 및 IP/MAC 설정이므로 다른 PC에서는 불필요 |
| `PEBoot\OneTimePE.ini` | **제외 권장** | 현재 PC의 BCD GUID 및 경로 설정 파일 |
| `PEBoot\OneTimePE.exe`, `boot.sdi` | 제외 가능 | 새 환경에서 빌드 시 자동으로 새로 생성·복사됨 (단, 빌드 없이 실행만 할 목적이면 복사) |

#### 현재 PC에서 클린 소스 패키지만 ZIP으로 묶기 (PowerShell)

프로젝트 루트 폴더에서 아래 명령을 실행하면 불필요한 캐시나 현재 PC 종속 설정 없이 **순수 빌드용 소스 압축 파일(`OneTimePE_Source.zip`)** 만 깔끔하게 생성할 수 있습니다.

```powershell
Compress-Archive -Path .\src, .\build.ps1, .\CMakeLists.txt, .\README.md, .\BUILD_GUIDE.md `
    -DestinationPath .\OneTimePE_Source.zip -Force
```

생성된 `OneTimePE_Source.zip`을 다른 환경의 원하는 경로(예: `C:\Projects\OneTimePE`)에 압축 해제합니다.

---

## 2. 빌드 도구(Toolchain) 준비하기

이 프로젝트의 기본 빌드 스크립트(`build.ps1`)는 **LLVM-MinGW (UCRT x86_64)** 환경을 기준으로 작성되어 있습니다. (`clang++` 및 `windres` 사용)

### 2-1. 온라인 환경에서 설치 (`winget` 권장)

PowerShell에서 아래 명령을 실행하면 최신 64비트 LLVM-MinGW가 자동으로 설치되고 사용자 `PATH`에 등록됩니다.

```powershell
winget install --id MartinStorsjo.LLVM-MinGW.UCRT -e
```

설치 완료 후 **PowerShell 창을 새로 열고** 도구가 정상 인식되는지 확인합니다.

```powershell
clang++ --version
windres --version
```

> 출력 내용 중 `Target: x86_64-w64-windows-gnu`가 표시되면 정상입니다.

---

### 2-2. 오프라인 / 포터블 환경에서 수동 설치 (무설치 ZIP)

인터넷이 차단된 환경이거나 시스템 설치 권한이 없는 경우, 포터블 압축 파일만으로 빌드 환경을 구성할 수 있습니다.

1. 인터넷이 되는 PC에서 [llvm-mingw 공식 릴리스 페이지](https://github.com/mstorsjo/llvm-mingw/releases)에 접속합니다.
2. **`llvm-mingw-<날짜>-ucrt-x86_64.zip`** 파일을 다운로드하여 타겟 PC로 복사한 뒤 원하는 폴더(예: `C:\Tools\llvm-mingw`)에 압축을 해제합니다.
3. 빌드할 PowerShell 창에서 아래와 같이 현재 세션의 `PATH`에 `bin` 폴더를 추가합니다. (시스템 환경 변수를 영구 수정하지 않아도 즉시 빌드 가능)

```powershell
$env:Path = "C:\Tools\llvm-mingw\bin;$env:Path"
clang++ --version
```

---

## 3. 프로젝트 빌드 실행하기

### 방법 1. `build.ps1` 원클릭 빌드 (가장 권장)

프로젝트 루트 폴더에서 아래 명령을 실행합니다.

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

#### `build.ps1` 내부 수행 과정
1. **`[1/3]` 리소스 컴파일 (`windres`)**
   * `src\app.rc` (아이콘, 관리자 권한 매니페스트, UTF-8 한글 버전 정보)를 컴파일하여 `build\app.res`를 생성합니다.
2. **`[2/3]` C++20 네이티브 정적 컴파일 (`clang++`)**
   * `src\main.cpp`, `src\net_sync.cpp`, `build\app.res`를 `-std=c++20 -O2 -municode -mwindows -static -s -Wl,--gc-sections` 옵션으로 컴파일 및 링크하여 **`PEBoot\OneTimePE.exe`** 를 생성합니다.
3. **`[3/3]` 내장 `boot.sdi` 준비**
   * `PEBoot\boot.sdi`가 없을 경우 현재 Windows 시스템(`C:\Windows\Boot\DVD\EFI\boot.sdi` 등)에서 표준 SDI 파일을 찾아 `PEBoot\boot.sdi`로 자동 복사합니다.

빌드가 완료되면 아래와 같은 메시지가 출력됩니다.

```text
Build succeeded! Output: ...\OneTimePE\PEBoot\OneTimePE.exe (538 KB)
```

---

### 방법 2. CMake를 이용한 빌드 (선택 사항)

CMake(`3.20` 이상)와 Ninja 또는 MinGW Makefiles 환경을 선호하는 경우 `CMakeLists.txt`로도 빌드할 수 있습니다.

```powershell
# CMake가 없는 경우 설치: winget install --id Kitware.CMake -e
cmake -B build-cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake
```

* Ninja가 설치된 경우 `-G "Ninja"`를 사용할 수 있습니다.
* CMake 빌드 산출물은 `build-cmake\OneTimePE.exe`에 생성되므로 실행 배포 폴더(`PEBoot\`)로 복사하여 사용합니다.
  ```powershell
  Copy-Item .\build-cmake\OneTimePE.exe .\PEBoot\OneTimePE.exe -Force
  ```

---

### 방법 3. 일반 MinGW-w64 (`g++`) 환경에서 수동 빌드

타겟 PC에 LLVM-MinGW(`clang++`) 대신 일반 MinGW-w64(WinLibs, MSYS2 UCRT64 등)의 `g++`와 `windres`가 설치되어 있다면, 아래 명령으로 동일하게 빌드할 수 있습니다.

```powershell
New-Item -ItemType Directory -Force -Path .\build, .\PEBoot | Out-Null

windres -I .\src -c 65001 .\src\app.rc -O coff -o .\build\app.res

g++ -std=c++20 -O2 -Wall -Wextra `
    -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 `
    -I.\src .\src\main.cpp .\src\net_sync.cpp .\build\app.res `
    -municode -mwindows -static -s -Wl,--gc-sections `
    -lcomctl32 -lole32 -lshell32 -luuid -lgdi32 -luser32 -liphlpapi `
    -o .\PEBoot\OneTimePE.exe
```

---

### (참고) Visual Studio (MSVC `cl.exe`)로 빌드 시 주의사항

Visual Studio의 **x64 Native Tools Command Prompt** 환경에서 MSVC로 빌드하려는 경우 아래 3가지 옵션을 반드시 지켜야 합니다.

1. **UTF-8 소스 인코딩 지정 (`/utf-8`)**: 소스 내 한국어 UI 문자열 깨짐 및 컴파일 오류 방지
2. **정적 CRT 런타임 링크 (`/MT`)**: VC++ 재배포 가능 패키지가 없는 WinPE 내부에서도 실행되도록 정적 링크 필수
3. **링커 자동 매니페스트 비활성화 (`/MANIFEST:NO`)**: `src\app.rc`에 이미 커스텀 UAC/DPI 매니페스트(`app.manifest`)가 포함되어 있으므로, 기본 매니페스트 생성을 끄지 않으면 리소스 중복 오류(`CVTRES` / `LNK1123`)가 발생함

```bat
rc /nologo /c65001 /I src /fo build\app.res src\app.rc
cl /nologo /std:c++20 /O2 /EHsc /MT /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 ^
   /I src src\main.cpp src\net_sync.cpp build\app.res ^
   /Fe:PEBoot\OneTimePE.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO comctl32.lib ole32.lib shell32.lib uuid.lib gdi32.lib user32.lib iphlpapi.lib ws2_32.lib
```

---

## 4. 빌드 결과 검증 및 실행 준비

빌드가 끝난 뒤 아래 항목을 확인합니다.

1. **산출물 확인**
   * `PEBoot\OneTimePE.exe` 파일이 생성되었는지 확인합니다.
   * `PEBoot\boot.sdi` 파일이 존재하는지 확인합니다. (없어도 `OneTimePE.exe` 실행 후 **[내장 SDI 생성]** 버튼으로 생성 가능)
2. **PE 이미지 배치**
   * 부팅에 사용할 WinPE 이미지(예: `boot.wim`)를 `PEBoot\` 폴더 안에 복사합니다.
3. **실행 확인**
   * `PEBoot\OneTimePE.exe`를 실행했을 때 **관리자 권한 승인(UAC)** 창이 뜨고, 메인 UI의 한글 텍스트·아이콘·현재 PC의 네트워크 어댑터 정보가 정상 표시되는지 확인합니다.

---

## 5. 자주 발생하는 문제 해결 (Troubleshooting)

| 증상 / 오류 메시지 | 원인 및 해결 방법 |
| :--- | :--- |
| `'windres'` 또는 `'clang++'` 용어가 인식되지 않음 | 컴파일러 설치 직후 환경 변수가 현재 터미널에 반영되지 않았거나 `PATH`에 누락된 상태입니다. PowerShell을 재시작하거나 `$env:Path = "<설치경로>\bin;$env:Path"`를 실행하세요. |
| `이 시스템에서 스크립트를 실행할 수 없으므로...` | Windows PowerShell 기본 실행 정책 제한입니다. `powershell -ExecutionPolicy Bypass -File .\build.ps1` 명령으로 실행하세요. |
| `Permission denied` (`ld.lld: error: cannot open output file ... OneTimePE.exe`) | 기존 `OneTimePE.exe`가 실행 중이거나 백신 실시간 감시가 파일을 잠시 잠근 상태입니다. 실행 중인 앱을 종료한 뒤 다시 빌드하세요. |
| 실행 시 Side-by-Side 구성 오류 발생 | 32비트(`i686`) 컴파일러로 빌드할 경우 매니페스트의 `processorArchitecture="amd64"`와 충돌합니다. 반드시 **64비트(`x86_64`)** 툴체인을 사용하세요. |
| 버전 정보 또는 UI 한글이 깨져 보임 | 소스 파일(`*.cpp`, `app.rc`)을 다른 에디터에서 저장하면서 인코딩이 `CP949(ANSI)`로 변경되었을 수 있습니다. 파일 인코딩을 **UTF-8**로 유지하고 `windres`에 `-c 65001` 옵션을 사용하세요. |

