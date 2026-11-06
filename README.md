# OneTimePE - 1회성 내장 WinPE 부팅 & 무인 네트워크 매니저

USB 이동식 드라이브 없이 내장 드라이브의 폴더에 PE 파일(`*.wim`, `*.sdi`)을 넣고, **다음 1회 재부팅만 WinPE로 부팅한 뒤 다시 재부팅하면 자동으로 기존 Windows로 복귀**하도록 해 주는 순수 Win32 C++20 네이티브 단일 실행 파일 앱입니다.

현재 실행 중인 Windows의 **네트워크 설정(유선/무선, 고정 IP·서브넷·게이트웨이·DNS, Wi-Fi 프로필 XML, 물리 랜카드 `.inf` 드라이버)** 을 자동으로 읽어들이고, `boot.wim`에 **무인 자동 실행 패치**를 적용하여 PE 부팅 직후 별도 조작 없이 인터넷/내부망이 즉시 연결되도록 지원합니다.

## 폴더 구조

```text
OneTimePE/
├── PEBoot/                      # 실제 실행 폴더 (어디로든 폴더째 이동 가능)
│   ├── OneTimePE.exe            # 독립 실행 가능한 Windows x64 네이티브 단일 앱 (PE 내부 엔진 겸용)
│   ├── boot.sdi                 # Windows 내장 표준 SDI 파일 (자동 준비됨)
│   ├── NetConfig/               # 네트워크 추출 시 자동 생성 (netconfig.ini, Drivers/, WiFi/)
│   └── (여기에 boot.wim 등 PE용 .wim 파일을 넣어주세요)
├── src/                         # Win32 네이티브 C++20 소스 및 리소스 파일
│   ├── main.cpp                 # Win32 GUI, BCD 제어, BitLocker/Secure Boot 진단
│   ├── net_sync.h               # 네트워크 감지/추출 및 WIM 무인 패치 인터페이스
│   ├── net_sync.cpp             # 어댑터/드라이버/Wi-Fi 추출, DISM WIM 패치, PE 자동 적용 엔진
│   ├── resource.h
│   ├── app.rc
│   ├── app.manifest
│   └── app.ico
├── CMakeLists.txt
└── build.ps1                    # 원클릭 빌드 스크립트
```

## 빠른 사용 방법

1. `PEBoot` 폴더 안에 보유하신 PE `.wim` 파일(예: PE ISO 안의 `sources\boot.wim`)을 복사해 넣습니다.
   - `boot.sdi`는 이미 `PEBoot` 폴더에 준비되어 있습니다. 다른 빈 폴더에 `OneTimePE.exe` 하나만 복사해 간 경우에도 앱 내 **[내장 SDI 생성]** 버튼을 누르면 Windows 내장 `boot.sdi`를 자동으로 생성해 줍니다.
2. `OneTimePE.exe`를 실행합니다 (관리자 권한 자동 요청).
3. 같은 폴더에 있는 `.wim` 및 `.sdi` 파일과 현재 연결된 Windows 네트워크 어댑터가 자동으로 인식됩니다.
4. **무인 네트워크 자동 연결 준비 (최초 1회)**:
   - **3번 섹션**의 **[WIM 무인패치 적용]** 버튼을 누르거나, 바로 **[▶ 저장 후 지금 PE 재부팅]** 버튼을 누른 뒤 안내 창에서 **[예]** 를 선택하면 `boot.wim` 내부에 무인 자동 실행 훅(`startnet.cmd`, `winpeshl.ini`, `Startup`)이 최초 1회 주입됩니다.
   - 이후에는 `boot.wim`을 다시 패치할 필요 없이, 부팅 예약 시마다 최신 네트워크 설정과 랜카드 드라이버가 `PEBoot\NetConfig\`에 자동 갱신됩니다.
5. **[▶ 저장 후 지금 PE 재부팅]** 버튼을 누르면 설정이 `OneTimePE.ini`에 저장되고, 1회성 부팅(`bcdedit /bootsequence`)이 예약된 뒤 재부팅됩니다.
6. **WinPE 부팅 완료 시**: 아무 조작 없이 백그라운드에서 자동으로 랜카드 드라이버(`drvload`) 적재 → 네트워크 초기화(`wpeutil InitializeNetwork`) → MAC 주소 기반 어댑터 매칭 → Wi-Fi 프로필 연결 및 고정 IP / DHCP / DNS 설정이 완료됩니다.
7. PE에서 작업을 마치고 재부팅하면 **기존 Windows로 자동 부팅**됩니다.

## 주요 기능 및 변수 설명

| 항목 | 설명 |
| --- | --- |
| **WIM / SDI 파일** | 같은 폴더(및 하위 `sources\`, `boot\`)의 `.wim`, `.sdi` 자동 탐색 및 드래그 앤 드롭 지원 |
| **부팅 이름 (`description`)** | BCD에 등록될 엔트리 설명 문자열 (기본값: `One-Time WinPE`) |
| **로더 경로 (`path`)** | 현재 시스템이 UEFI인지 Legacy BIOS인지 자동 감지하여 `winload.efi` / `winload.exe` 기본 선택 |
| **SystemRoot / NX 정책** | PE 내부 Windows 루트(`\windows`), `winpe yes`, `detecthal yes`, DEP 정책(`OptIn` 등) 지정 |
| **보안 / 암호화 진단** | 대상 드라이브 및 전체 드라이브의 **BitLocker 설정 여부**와 **UEFI Secure Boot 켜짐/꺼짐 상태** 실시간 표시 및 **[BitLocker 상세 확인]** 리포트 제공 |
| **BitLocker 일시 중지** | 암호화 보호가 켜진 드라이브일 경우 지정한 재부팅 횟수(기본 `2`회) 동안 `manage-bde`로 보호 자동 일시 중지 |
| **무인 네트워크 동기화** | 물리 NIC(Hyper-V 외부 가상 스위치 브리지 환경 포함) 자동 식별, 고정 IP/DHCP 및 수동/자동 DNS 판별, `pnputil` 랜카드 드라이버 추출, `netsh wlan` Wi-Fi 프로필 XML 추출 |
| **WIM 무인 자동 실행 패치** | DISM으로 `boot.wim`을 마운트하여 표준 PE(`startnet.cmd`)와 커스텀 PECMD/GUI PE(`winpeshl.ini`, `Startup`) 모두에서 `OneTimePE.exe --pe-apply-net`이 1회 자동 실행되도록 패치 |
| **추가 변수** | 세미콜론(`;`)으로 구분하여 추가 `bcdedit /set {GUID} <변수>` 명령 지정 가능 |

## 직접 다시 빌드하기 (`clang++` / `g++` / Visual Studio `cl.exe` 모두 지원)

LLVM-MinGW(`clang++`), 일반 MinGW-w64(`g++`), 또는 Visual Studio(`cl.exe`)가 설치된 환경에서 아래 명령을 실행하면 `PEBoot\OneTimePE.exe`가 생성됩니다.

```powershell
# 로컬 소스 빌드
powershell -ExecutionPolicy Bypass -File .\build.ps1

# 다른 PC(Git 복제 환경)에서 최신 원격 커밋 강제 동기화 후 빌드
powershell -ExecutionPolicy Bypass -File .\build.ps1 -Sync
```
