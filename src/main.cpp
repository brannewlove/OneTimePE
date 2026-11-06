#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <winioctl.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <thread>

#include "resource.h"
#include "net_sync.h"

#define OTPE_WIDE_IMPL(x) L##x
#define OTPE_WIDE(x)      OTPE_WIDE_IMPL(x)

namespace {

constexpr wchar_t kAppVersion[]      = L"v1.3";
constexpr wchar_t kBuildTimestamp[]  = OTPE_WIDE(__DATE__) L" " OTPE_WIDE(__TIME__);
constexpr wchar_t kWindowClassName[] = L"OneTimePE_MainWindow";
constexpr wchar_t kWindowTitle[]     = L"OneTimePE v1.3 (커스텀PE & C: 자동복구) - 1회성 WinPE & 무인 부팅 매니저";
constexpr wchar_t kIniSection[]      = L"OneTimePE";

struct AppConfig {
    std::wstring wimInput           = L"boot.wim";
    std::wstring sdiInput           = L"boot.sdi";
    std::wstring description        = L"One-Time WinPE";
    std::wstring loaderPath         = L"";
    std::wstring systemRoot         = L"\\windows";
    bool winpeMode                  = true;
    bool detectHal                  = true;
    std::wstring nxPolicy           = L"";
    bool suspendBitLocker           = false;
    int bitLockerRebootCount        = 2;
    int rebootDelaySec              = 3;
    std::wstring extraSetArgs       = L"";
    bool autoApplyNetwork           = true;
    bool exportNicDrivers           = true;
    bool importPatchDriveFix        = true;
    std::wstring patchedWimPath     = L"";
    std::wstring patchedWimSig      = L"";
    std::wstring sdiGuid            = L"";
    std::wstring peGuid             = L"";
};

struct ResolvedFileInfo {
    bool exists                = false;
    std::wstring fullPath;
    std::wstring driveColon;   // e.g. "C:"
    std::wstring bcdRelPath;   // e.g. "\PEBoot\boot.wim"
    ULONGLONG sizeBytes        = 0;
    bool isEncryptedEfs        = false;
    bool isCompressedNtfs      = false;
    UINT driveType             = DRIVE_UNKNOWN;
    STORAGE_BUS_TYPE busType   = BusTypeUnknown;
    std::wstring fileSystem;   // e.g. "NTFS", "FAT32", "exFAT"
    int bitLockerState         = -1;
};

enum class PostPatchFollowUp {
    None,
    ContinueApplyBcdOnly,
    ContinueQueueOnly,
    ContinueBootNow
};

enum class AsyncWimTaskKind {
    UnattendedNetPatch,
    CustomImportDriveFixPatch,
    ConvertEsdToWim
};

struct AsyncWimTaskResult {
    AsyncWimTaskKind  kind           = AsyncWimTaskKind::UnattendedNetPatch;
    bool              success        = false;
    std::wstring      wimPath;
    std::wstring      newSig;
    PostPatchFollowUp followUp       = PostPatchFollowUp::None;
    std::wstring      customSummary;
    bool              warnSecureBoot = false;
};

// Global UI state
HINSTANCE g_hInst          = nullptr;
HWND      g_hMainWnd       = nullptr;
UINT      g_dpi            = 96;
HFONT     g_hFontNormal    = nullptr;
HFONT     g_hFontBold      = nullptr;
HFONT     g_hFontTitle     = nullptr;
HFONT     g_hFontMono      = nullptr;
HBRUSH    g_hBgBrush       = nullptr;

std::wstring g_appDir;
std::wstring g_iniPath;
std::wstring g_machineId;
bool         g_resetFromOtherPc = false;
bool         g_isUefi           = true;
int          g_secureBootState  = -1;
bool         g_isAdmin          = false;
bool         g_isTaskBusy       = false;
AppConfig    g_cfg;

// Controls
HWND g_hLblTitle      = nullptr;
HWND g_hLblSysInfo    = nullptr;

HWND g_hGrpFiles        = nullptr;
HWND g_hLblAppDir       = nullptr;
HWND g_hEdtAppDir       = nullptr;
HWND g_hBtnImportCustom = nullptr;
HWND g_hBtnOpenDir      = nullptr;
HWND g_hBtnRescan       = nullptr;
HWND g_hLblWim        = nullptr;
HWND g_hCmbWim        = nullptr;
HWND g_hBtnBrowseWim  = nullptr;
HWND g_hLblSdi        = nullptr;
HWND g_hCmbSdi        = nullptr;
HWND g_hBtnBrowseSdi  = nullptr;
HWND g_hBtnGenSdi     = nullptr;
HWND g_hLblFileStatus = nullptr;

HWND g_hGrpVars       = nullptr;
HWND g_hLblDesc       = nullptr;
HWND g_hEdtDesc       = nullptr;
HWND g_hLblSysRoot    = nullptr;
HWND g_hEdtSysRoot    = nullptr;
HWND g_hLblLoader     = nullptr;
HWND g_hCmbLoader     = nullptr;
HWND g_hChkWinpe      = nullptr;
HWND g_hChkDetectHal  = nullptr;
HWND g_hLblNx         = nullptr;
HWND g_hCmbNx         = nullptr;
HWND g_hLblSecTitle   = nullptr;
HWND g_hLblSecStatus  = nullptr;
HWND g_hBtnCheckBl    = nullptr;
HWND g_hChkBitLocker  = nullptr;
HWND g_hLblBlCount    = nullptr;
HWND g_hEdtBlCount    = nullptr;
HWND g_hLblRebootSec  = nullptr;
HWND g_hEdtRebootSec  = nullptr;
HWND g_hLblExtra      = nullptr;
HWND g_hEdtExtra      = nullptr;

HWND g_hGrpNet        = nullptr;
HWND g_hLblNetStatus  = nullptr;
HWND g_hChkAutoNet    = nullptr;
HWND g_hChkExportDrv  = nullptr;
HWND g_hBtnSyncNet    = nullptr;
HWND g_hBtnPatchWim   = nullptr;

HWND g_hGrpActions    = nullptr;
HWND g_hLblBcdStatus  = nullptr;
HWND g_hBtnRefreshBcd = nullptr;
HWND g_hBtnSaveCfg    = nullptr;
HWND g_hBtnApplyBcd   = nullptr;
HWND g_hBtnQueueOnly  = nullptr;
HWND g_hBtnCleanBcd   = nullptr;
HWND g_hBtnBootNow    = nullptr;

HWND g_hGrpLog        = nullptr;
HWND g_hBtnClearLog   = nullptr;
HWND g_hEdtLog        = nullptr;

int Scale(int px) {
    return MulDiv(px, static_cast<int>(g_dpi), 96);
}

std::wstring Trim(const std::wstring& s) {
    size_t start = 0;
    while (start < s.size() && std::iswspace(s[start])) ++start;
    size_t end = s.size();
    while (end > start && std::iswspace(s[end - 1])) --end;
    return s.substr(start, end - start);
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring JoinPath(const std::wstring& base, const std::wstring& rel) {
    if (base.empty()) return rel;
    if (rel.empty()) return base;
    if (base.back() == L'\\' || base.back() == L'/') {
        if (rel.front() == L'\\' || rel.front() == L'/') return base + rel.substr(1);
        return base + rel;
    }
    if (rel.front() == L'\\' || rel.front() == L'/') return base + rel;
    return base + L"\\" + rel;
}

bool IsAbsolutePath(const std::wstring& path) {
    if (path.size() >= 2 && std::iswalpha(path[0]) && path[1] == L':') return true;
    if (path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/')) return true;
    return false;
}

std::wstring MakeAppRelativeIfInside(const std::wstring& fullPath) {
    std::wstring lowFull = ToLower(fullPath);
    std::wstring lowApp  = ToLower(g_appDir);
    if (!lowApp.empty() && lowApp.back() != L'\\') lowApp += L'\\';
    if (lowFull.rfind(lowApp, 0) == 0 && fullPath.size() > lowApp.size()) {
        return fullPath.substr(lowApp.size());
    }
    return fullPath;
}

std::wstring GetWndText(HWND hWnd) {
    if (!hWnd) return L"";
    int len = GetWindowTextLengthW(hWnd);
    if (len <= 0) return L"";
    std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hWnd, buf.data(), len + 1);
    buf.resize(static_cast<size_t>(len));
    return buf;
}

void AppendLog(const std::wstring& msg) {
    if (!g_hEdtLog) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t timeBuf[32];
    swprintf_s(timeBuf, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);

    std::wstring formatted = timeBuf;
    for (size_t i = 0; i < msg.size(); ++i) {
        if (msg[i] == L'\r') {
            if (i + 1 < msg.size() && msg[i + 1] == L'\n') ++i;
            formatted += L"\r\n";
        } else if (msg[i] == L'\n') {
            formatted += L"\r\n";
        } else {
            formatted += msg[i];
        }
    }
    if (formatted.size() < 2 || formatted.substr(formatted.size() - 2) != L"\r\n") {
        formatted += L"\r\n";
    }

    int endPos = GetWindowTextLengthW(g_hEdtLog);
    SendMessageW(g_hEdtLog, EM_SETSEL, static_cast<WPARAM>(endPos), static_cast<LPARAM>(endPos));
    SendMessageW(g_hEdtLog, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(formatted.c_str()));
    SendMessageW(g_hEdtLog, EM_SCROLLCARET, 0, 0);
}

void PostLogFromWorkerThread(const std::wstring& msg) {
    if (!g_hMainWnd) return;
    auto* heapStr = new std::wstring(msg);
    if (!PostMessageW(g_hMainWnd, WM_APP_TASK_LOG, 0, reinterpret_cast<LPARAM>(heapStr))) {
        delete heapStr;
    }
}

void SetTaskBusyState(bool busy, const wchar_t* customBtnText = nullptr) {
    g_isTaskBusy = busy;
    const HWND interactive[] = {
        g_hBtnImportCustom, g_hCmbWim, g_hBtnBrowseWim, g_hCmbSdi, g_hBtnBrowseSdi, g_hBtnGenSdi, g_hBtnRescan,
        g_hBtnCheckBl, g_hBtnSyncNet, g_hBtnPatchWim,
        g_hBtnRefreshBcd, g_hBtnSaveCfg, g_hBtnApplyBcd, g_hBtnQueueOnly, g_hBtnCleanBcd, g_hBtnBootNow
    };
    for (HWND h : interactive) {
        if (h) EnableWindow(h, busy ? FALSE : TRUE);
    }
    if (busy) {
        SetWindowTextW(g_hBtnPatchWim, customBtnText ? customBtnText : L"WIM 작업 진행중...");
    } else {
        SetWindowTextW(g_hBtnPatchWim, L"WIM 무인패치 적용");
    }
}

bool DetectFirmwareIsUefi() {
    FIRMWARE_TYPE ft = FirmwareTypeUnknown;
    if (GetFirmwareType(&ft)) {
        if (ft == FirmwareTypeUefi) return true;
        if (ft == FirmwareTypeBios) return false;
    }
    wchar_t envBuf[64]{};
    if (GetEnvironmentVariableW(L"firmware_type", envBuf, 64) > 0) {
        if (_wcsicmp(envBuf, L"UEFI") == 0) return true;
        if (_wcsicmp(envBuf, L"Legacy") == 0 || _wcsicmp(envBuf, L"BIOS") == 0) return false;
    }
    return true;
}

int DetectSecureBootState() {
    if (!g_isUefi) return -1;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return -1;
    }
    DWORD val  = 0;
    DWORD cb   = sizeof(val);
    DWORD type = 0;
    LONG st = RegQueryValueExW(hKey, L"UEFISecureBootEnabled", nullptr, &type,
                               reinterpret_cast<LPBYTE>(&val), &cb);
    RegCloseKey(hKey);
    if (st == ERROR_SUCCESS && type == REG_DWORD) {
        return (val != 0) ? 1 : 0;
    }
    return -1;
}

std::wstring DescribeBitLockerState(int state) {
    switch (state) {
        case 0: return L"미설정 (암호화 안 됨)";
        case 1: return L"설정됨 — 보호 켜짐! (일시중지 필요)";
        case 2: return L"미설정 (해독됨)";
        case 3: return L"설정됨 — 암호화 진행 중";
        case 4: return L"해독 진행 중";
        case 5: return L"설정됨 — 보호 일시중지됨 (부팅 가능)";
        case 6: return L"설정됨 — 볼륨 잠김!";
        case 8: return L"활성화 대기 중 (보호 꺼짐)";
        default: return L"상태 미확인";
    }
}

bool DetectIsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
}

// Dynamically load propsys.dll so OneTimePE.exe has zero static dependency on propsys.dll inside minimal WinPE
int QueryVolumeBitLockerState(const std::wstring& driveColon) {
    if (driveColon.empty()) return -1;
    static HMODULE hPropSys = LoadLibraryW(L"propsys.dll");
    if (!hPropSys) return -1;

    using FnPSGetPropertyKeyFromName = HRESULT(WINAPI*)(PCWSTR, PROPERTYKEY*);
    static auto pGetKey = reinterpret_cast<FnPSGetPropertyKeyFromName>(
        reinterpret_cast<void*>(GetProcAddress(hPropSys, "PSGetPropertyKeyFromName")));
    if (!pGetKey) return -1;

    PROPERTYKEY key{};
    if (FAILED(pGetKey(L"System.Volume.BitLockerProtection", &key))) return -1;

    std::wstring rootPath = driveColon + L"\\";
    IShellItem2* pItem = nullptr;
    if (FAILED(SHCreateItemFromParsingName(rootPath.c_str(), nullptr, IID_PPV_ARGS(&pItem))) || !pItem) {
        return -1;
    }

    ULONG val = 0;
    HRESULT hr = pItem->GetUInt32(key, &val);
    pItem->Release();
    return SUCCEEDED(hr) ? static_cast<int>(val) : -1;
}

std::wstring GetSystemDriveColon() {
    wchar_t winDir[MAX_PATH]{};
    if (GetWindowsDirectoryW(winDir, MAX_PATH) >= 2 && std::iswalpha(winDir[0]) && winDir[1] == L':') {
        std::wstring d(winDir, 2);
        d[0] = static_cast<wchar_t>(std::towupper(d[0]));
        return d;
    }
    return L"C:";
}

std::wstring GetLocalMachineId() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography",
                      0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        wchar_t guidBuf[128]{};
        DWORD cb = sizeof(guidBuf);
        DWORD type = 0;
        LONG st = RegQueryValueExW(hKey, L"MachineGuid", nullptr, &type,
                                   reinterpret_cast<LPBYTE>(guidBuf), &cb);
        RegCloseKey(hKey);
        if (st == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && guidBuf[0] != L'\0') {
            return Trim(guidBuf);
        }
    }
    wchar_t compBuf[MAX_COMPUTERNAME_LENGTH + 4]{};
    DWORD len = _countof(compBuf);
    if (GetComputerNameW(compBuf, &len) && len > 0) {
        return std::wstring(compBuf, len);
    }
    return L"";
}

STORAGE_BUS_TYPE QueryVolumeStorageBusType(const std::wstring& driveColon) {
    if (driveColon.size() < 2) return BusTypeUnknown;
    std::wstring devPath = L"\\\\.\\" + driveColon.substr(0, 2);
    HANDLE hVol = CreateFileW(devPath.c_str(), 0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) return BusTypeUnknown;

    STORAGE_PROPERTY_QUERY spq{};
    spq.PropertyId = StorageDeviceProperty;
    spq.QueryType  = PropertyStandardQuery;

    BYTE outBuf[512]{};
    DWORD retBytes = 0;
    STORAGE_BUS_TYPE bt = BusTypeUnknown;
    if (DeviceIoControl(hVol, IOCTL_STORAGE_QUERY_PROPERTY, &spq, sizeof(spq),
                        outBuf, sizeof(outBuf), &retBytes, nullptr) &&
        retBytes >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(outBuf);
        bt = desc->BusType;
    }
    CloseHandle(hVol);
    return bt;
}

bool IsExternalUsbOrCardBus(STORAGE_BUS_TYPE bt) {
    return (bt == BusTypeUsb || bt == BusTypeSd || bt == BusTypeMmc || bt == BusType1394);
}

bool IsFileBackedVirtualBus(STORAGE_BUS_TYPE bt) {
    return (bt == BusTypeFileBackedVirtual);
}

bool IsBootSupportedFileSystem(const std::wstring& fs) {
    if (fs.empty()) return true;
    return (_wcsicmp(fs.c_str(), L"NTFS") == 0 ||
            _wcsicmp(fs.c_str(), L"FAT32") == 0 ||
            _wcsicmp(fs.c_str(), L"FAT") == 0);
}

bool HasValidSdiSignature(const std::wstring& sdiPath) {
    HANDLE h = CreateFileW(sdiPath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char magic[8]{};
    DWORD readBytes = 0;
    BOOL ok = ReadFile(h, magic, sizeof(magic), &readBytes, nullptr);
    CloseHandle(h);
    if (!ok || readBytes < 8) return false;
    return std::memcmp(magic, "$SDI0001", 8) == 0;
}

std::wstring NormalizeLoaderPathForFirmware(const std::wstring& rawLoader, bool isUefi) {
    std::wstring loader = Trim(rawLoader);
    if (loader.empty()) {
        return isUefi ? L"\\windows\\system32\\winload.efi"
                      : L"\\windows\\system32\\winload.exe";
    }
    for (auto& ch : loader) {
        if (ch == L'/') ch = L'\\';
    }
    if (loader.front() != L'\\') loader = L"\\" + loader;

    std::wstring low = ToLower(loader);
    if (isUefi && low.size() >= 4 && low.substr(low.size() - 4) == L".exe") {
        loader = loader.substr(0, loader.size() - 4) + L".efi";
    } else if (!isUefi && low.size() >= 4 && low.substr(low.size() - 4) == L".efi") {
        loader = loader.substr(0, loader.size() - 4) + L".exe";
    }
    return loader;
}

std::wstring GetNormalizedBcdRelativeDir(const std::wstring& dirPath) {
    HANDLE hDir = CreateFileW(dirPath.c_str(), 0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    std::wstring target = dirPath;
    if (hDir != INVALID_HANDLE_VALUE) {
        wchar_t finalBuf[MAX_PATH * 4]{};
        DWORD len = GetFinalPathNameByHandleW(hDir, finalBuf, _countof(finalBuf),
                                              FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        CloseHandle(hDir);
        if (len > 0 && len < _countof(finalBuf)) {
            std::wstring norm(finalBuf, len);
            if (norm.rfind(L"\\\\?\\", 0) == 0) norm = norm.substr(4);
            target = norm;
        }
    }
    if (target.size() >= 3 && std::iswalpha(target[0]) && target[1] == L':') {
        std::wstring rel = target.substr(2);
        while (rel.size() > 1 && (rel.back() == L'\\' || rel.back() == L'/')) rel.pop_back();
        return rel;
    }
    return L"";
}

ResolvedFileInfo ResolvePeFile(const std::wstring& rawInput) {
    ResolvedFileInfo info{};
    std::wstring trimmed = Trim(rawInput);
    if (trimmed.empty()) return info;

    std::wstring target = IsAbsolutePath(trimmed) ? trimmed : JoinPath(g_appDir, trimmed);

    DWORD attrs = GetFileAttributesW(target.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        info.fullPath = target;
        return info;
    }

    info.exists = true;
    info.isEncryptedEfs   = (attrs & FILE_ATTRIBUTE_ENCRYPTED) != 0;
    info.isCompressedNtfs = (attrs & FILE_ATTRIBUTE_COMPRESSED) != 0;

    HANDLE hFile = CreateFileW(target.c_str(), 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER fileSize{};
        if (GetFileSizeEx(hFile, &fileSize)) {
            info.sizeBytes = static_cast<ULONGLONG>(fileSize.QuadPart);
        }
        wchar_t finalBuf[MAX_PATH * 4]{};
        DWORD len = GetFinalPathNameByHandleW(hFile, finalBuf, _countof(finalBuf),
                                              FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        CloseHandle(hFile);

        if (len > 0 && len < _countof(finalBuf)) {
            std::wstring normalized(finalBuf, len);
            if (normalized.rfind(L"\\\\?\\", 0) == 0) {
                normalized = normalized.substr(4);
            }
            target = normalized;
        }
    }

    info.fullPath = target;
    if (target.size() >= 3 && std::iswalpha(target[0]) && target[1] == L':' &&
        (target[2] == L'\\' || target[2] == L'/')) {
        info.driveColon = target.substr(0, 2);
        info.driveColon[0] = static_cast<wchar_t>(std::towupper(info.driveColon[0]));
        info.bcdRelPath = target.substr(2);
        for (auto& ch : info.bcdRelPath) {
            if (ch == L'/') ch = L'\\';
        }
        std::wstring rootSlash = info.driveColon + L"\\";
        info.driveType = GetDriveTypeW(rootSlash.c_str());
        info.busType   = QueryVolumeStorageBusType(info.driveColon);
        wchar_t fsBuf[64]{};
        if (GetVolumeInformationW(rootSlash.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
                                  fsBuf, _countof(fsBuf)) && fsBuf[0] != L'\0') {
            info.fileSystem = fsBuf;
        }
        info.bitLockerState = QueryVolumeBitLockerState(info.driveColon);
    }
    return info;
}

bool IsWimCurrentlyPatched(const ResolvedFileInfo& wimInfo) {
    if (!wimInfo.exists) {
        return false;
    }
    if (!g_cfg.patchedWimSig.empty()) {
        std::wstring curSig = net_sync::ComputeFileSignature(wimInfo.fullPath);
        if (!curSig.empty() && curSig == g_cfg.patchedWimSig) {
            return true;
        }
    }
    return net_sync::IsWimUnattendedPatched(wimInfo.fullPath, 0);
}

std::wstring DecodeConsoleOutput(const std::string& bytes) {
    if (bytes.empty()) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                   static_cast<int>(bytes.size()), nullptr, 0);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (wlen <= 0) {
        cp = GetOEMCP();
        flags = 0;
        wlen = MultiByteToWideChar(cp, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    }
    if (wlen <= 0) return L"";
    std::wstring out(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(cp, flags, bytes.data(), static_cast<int>(bytes.size()), out.data(), wlen);
    return out;
}

struct CmdResult {
    DWORD exitCode = static_cast<DWORD>(-1);
    std::wstring output;
};

CmdResult RunSystemTool(const std::wstring& exeName, const std::wstring& args, bool logCommand = true) {
    CmdResult res{};
    wchar_t sysDir[MAX_PATH]{};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring exePath = JoinPath(sysDir, exeName);
    if (GetFileAttributesW(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        exePath = exeName;
    }

    std::wstring cmdLine = L"\"" + exePath + L"\" " + args;
    if (logCommand) {
        AppendLog(L"> " + exeName + L" " + args);
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr;
    HANDLE hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        res.output = L"파이프 생성 실패";
        if (logCommand) AppendLog(res.output);
        return res;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, g_appDir.c_str(), &si, &pi);
    CloseHandle(hWrite);

    if (!ok) {
        DWORD err = GetLastError();
        CloseHandle(hRead);
        wchar_t errBuf[128];
        swprintf_s(errBuf, L"프로세스 실행 실패 (오류 코드: %lu)", err);
        res.output = errBuf;
        if (logCommand) AppendLog(res.output);
        return res;
    }

    std::string rawOut;
    char chunk[2048];
    DWORD bytesRead = 0;
    while (ReadFile(hRead, chunk, sizeof(chunk), &bytesRead, nullptr) && bytesRead > 0) {
        rawOut.append(chunk, bytesRead);
    }
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &res.exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    res.output = Trim(DecodeConsoleOutput(rawOut));
    if (logCommand && !res.output.empty()) {
        AppendLog(res.output);
    }
    return res;
}

std::wstring ExtractFirstGuid(const std::wstring& text) {
    for (size_t i = 0; i + 37 < text.size(); ++i) {
        if (text[i] == L'{' && text[i + 37] == L'}') {
            bool valid = true;
            for (size_t j = 1; j < 37; ++j) {
                wchar_t c = text[i + j];
                if (j == 9 || j == 14 || j == 19 || j == 24) {
                    if (c != L'-') { valid = false; break; }
                } else {
                    if (!std::iswxdigit(c)) { valid = false; break; }
                }
            }
            if (valid) return text.substr(i, 38);
        }
    }
    return L"";
}

bool DoesBcdGuidExist(const std::wstring& guid) {
    if (guid.empty()) return false;
    CmdResult r = RunSystemTool(L"bcdedit.exe", L"/enum \"" + guid + L"\" /v", false);
    return r.exitCode == 0;
}

bool IsGuidInBootSequence(const std::wstring& guid) {
    if (guid.empty()) return false;
    CmdResult r = RunSystemTool(L"bcdedit.exe", L"/enum {bootmgr} /v", false);
    if (r.exitCode != 0) return false;

    std::wstring lowOut  = ToLower(r.output);
    std::wstring lowGuid = ToLower(guid);

    size_t pos = lowOut.find(L"bootsequence");
    if (pos == std::wstring::npos) return false;

    size_t lineEnd = lowOut.find(L'\n', pos);
    std::wstring firstLine = (lineEnd == std::wstring::npos) ? lowOut.substr(pos) : lowOut.substr(pos, lineEnd - pos);
    if (firstLine.find(lowGuid) != std::wstring::npos) return true;

    size_t cur = (lineEnd == std::wstring::npos) ? lowOut.size() : lineEnd + 1;
    while (cur < lowOut.size()) {
        size_t nextNl = lowOut.find(L'\n', cur);
        std::wstring line = (nextNl == std::wstring::npos) ? lowOut.substr(cur) : lowOut.substr(cur, nextNl - cur);
        if (!line.empty() && !std::iswspace(line[0])) break;
        if (line.find(lowGuid) != std::wstring::npos) return true;
        if (nextNl == std::wstring::npos) break;
        cur = nextNl + 1;
    }
    return false;
}

void EnsureUnicodeIniFileExists() {
    if (GetFileAttributesW(g_iniPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE hFile = CreateFileW(g_iniPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        const BYTE bom[2] = { 0xFF, 0xFE };
        DWORD written = 0;
        WriteFile(hFile, bom, sizeof(bom), &written, nullptr);
        CloseHandle(hFile);
    }
}

std::wstring ReadIniStr(const wchar_t* key, const std::wstring& defVal) {
    wchar_t buf[2048]{};
    GetPrivateProfileStringW(kIniSection, key, defVal.c_str(), buf, _countof(buf), g_iniPath.c_str());
    return buf;
}

int ReadIniInt(const wchar_t* key, int defVal) {
    return static_cast<int>(GetPrivateProfileIntW(kIniSection, key, defVal, g_iniPath.c_str()));
}

void WriteIniStr(const wchar_t* key, const std::wstring& val) {
    WritePrivateProfileStringW(kIniSection, key, val.c_str(), g_iniPath.c_str());
}

void LoadConfigFromIni() {
    std::wstring defaultLoader = g_isUefi ? L"\\windows\\system32\\winload.efi"
                                          : L"\\windows\\system32\\winload.exe";
    if (GetFileAttributesW(g_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_cfg.loaderPath = defaultLoader;
        return;
    }

    std::wstring savedMachineId = ReadIniStr(L"MachineId", L"");
    g_cfg.wimInput             = ReadIniStr(L"WimFile", L"boot.wim");
    g_cfg.sdiInput             = ReadIniStr(L"SdiFile", L"boot.sdi");
    g_cfg.description          = ReadIniStr(L"Description", L"One-Time WinPE");
    g_cfg.loaderPath           = NormalizeLoaderPathForFirmware(ReadIniStr(L"LoaderPath", defaultLoader), g_isUefi);
    g_cfg.systemRoot           = ReadIniStr(L"SystemRoot", L"\\windows");
    g_cfg.winpeMode            = ReadIniInt(L"WinPEMode", 1) != 0;
    g_cfg.detectHal            = ReadIniInt(L"DetectHal", 1) != 0;
    g_cfg.nxPolicy             = ReadIniStr(L"NxPolicy", L"");
    g_cfg.suspendBitLocker     = ReadIniInt(L"SuspendBitLocker", 0) != 0;
    g_cfg.bitLockerRebootCount = std::clamp(ReadIniInt(L"BitLockerRebootCount", 2), 0, 15);
    g_cfg.rebootDelaySec       = std::clamp(ReadIniInt(L"RebootDelaySec", 3), 0, 300);
    g_cfg.extraSetArgs         = ReadIniStr(L"ExtraSetArgs", L"");
    g_cfg.autoApplyNetwork     = ReadIniInt(L"AutoApplyNetwork", 1) != 0;
    g_cfg.exportNicDrivers     = ReadIniInt(L"ExportNicDrivers", 1) != 0;
    g_cfg.importPatchDriveFix  = ReadIniInt(L"ImportPatchDriveFix", 1) != 0;
    g_cfg.patchedWimPath       = ReadIniStr(L"PatchedWimPath", L"");
    g_cfg.patchedWimSig        = ReadIniStr(L"PatchedWimSig", L"");
    g_cfg.sdiGuid              = ReadIniStr(L"SdiGuid", L"");
    g_cfg.peGuid               = ReadIniStr(L"PeGuid", L"");

    // If copied from another PC, clear stale BCD GUIDs and reset loader path to match current firmware
    if (!g_machineId.empty() && !savedMachineId.empty() &&
        _wcsicmp(savedMachineId.c_str(), g_machineId.c_str()) != 0) {
        g_cfg.sdiGuid.clear();
        g_cfg.peGuid.clear();
        g_cfg.loaderPath = defaultLoader;
        if (ToLower(g_cfg.wimInput).find(L"onetimepe_cache") != std::wstring::npos) {
            g_cfg.wimInput = L"boot.wim";
        }
        if (ToLower(g_cfg.sdiInput).find(L"onetimepe_cache") != std::wstring::npos) {
            g_cfg.sdiInput = L"boot.sdi";
        }
        g_resetFromOtherPc = true;
    }
}

void PullControlsToConfig() {
    g_cfg.wimInput    = Trim(GetWndText(g_hCmbWim));
    g_cfg.sdiInput    = Trim(GetWndText(g_hCmbSdi));
    g_cfg.description = Trim(GetWndText(g_hEdtDesc));
    if (g_cfg.description.empty()) g_cfg.description = L"One-Time WinPE";

    g_cfg.loaderPath  = Trim(GetWndText(g_hCmbLoader));
    if (g_cfg.loaderPath.empty()) {
        g_cfg.loaderPath = g_isUefi ? L"\\windows\\system32\\winload.efi"
                                    : L"\\windows\\system32\\winload.exe";
    }

    g_cfg.systemRoot  = Trim(GetWndText(g_hEdtSysRoot));
    if (g_cfg.systemRoot.empty()) g_cfg.systemRoot = L"\\windows";

    g_cfg.winpeMode   = (SendMessageW(g_hChkWinpe, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_cfg.detectHal   = (SendMessageW(g_hChkDetectHal, BM_GETCHECK, 0, 0) == BST_CHECKED);

    int nxIdx = static_cast<int>(SendMessageW(g_hCmbNx, CB_GETCURSEL, 0, 0));
    switch (nxIdx) {
        case 1: g_cfg.nxPolicy = L"OptIn"; break;
        case 2: g_cfg.nxPolicy = L"OptOut"; break;
        case 3: g_cfg.nxPolicy = L"AlwaysOff"; break;
        case 4: g_cfg.nxPolicy = L"AlwaysOn"; break;
        default: g_cfg.nxPolicy = L""; break;
    }

    g_cfg.suspendBitLocker = (SendMessageW(g_hChkBitLocker, BM_GETCHECK, 0, 0) == BST_CHECKED);

    std::wstring blStr = Trim(GetWndText(g_hEdtBlCount));
    g_cfg.bitLockerRebootCount = blStr.empty() ? 2 : std::clamp(_wtoi(blStr.c_str()), 0, 15);

    std::wstring rbStr = Trim(GetWndText(g_hEdtRebootSec));
    g_cfg.rebootDelaySec = rbStr.empty() ? 3 : std::clamp(_wtoi(rbStr.c_str()), 0, 300);

    g_cfg.extraSetArgs     = Trim(GetWndText(g_hEdtExtra));
    g_cfg.autoApplyNetwork = (SendMessageW(g_hChkAutoNet, BM_GETCHECK, 0, 0) == BST_CHECKED);
    g_cfg.exportNicDrivers = (SendMessageW(g_hChkExportDrv, BM_GETCHECK, 0, 0) == BST_CHECKED);
}

bool SaveConfigToIni(bool logSaved = true) {
    PullControlsToConfig();
    EnsureUnicodeIniFileExists();

    if (!g_machineId.empty()) {
        WriteIniStr(L"MachineId", g_machineId);
    }
    WriteIniStr(L"WimFile", g_cfg.wimInput);
    WriteIniStr(L"SdiFile", g_cfg.sdiInput);
    WriteIniStr(L"Description", g_cfg.description);
    WriteIniStr(L"LoaderPath", g_cfg.loaderPath);
    WriteIniStr(L"SystemRoot", g_cfg.systemRoot);
    WriteIniStr(L"WinPEMode", g_cfg.winpeMode ? L"1" : L"0");
    WriteIniStr(L"DetectHal", g_cfg.detectHal ? L"1" : L"0");
    WriteIniStr(L"NxPolicy", g_cfg.nxPolicy);
    WriteIniStr(L"SuspendBitLocker", g_cfg.suspendBitLocker ? L"1" : L"0");
    WriteIniStr(L"BitLockerRebootCount", std::to_wstring(g_cfg.bitLockerRebootCount));
    WriteIniStr(L"RebootDelaySec", std::to_wstring(g_cfg.rebootDelaySec));
    WriteIniStr(L"ExtraSetArgs", g_cfg.extraSetArgs);
    WriteIniStr(L"AutoApplyNetwork", g_cfg.autoApplyNetwork ? L"1" : L"0");
    WriteIniStr(L"ExportNicDrivers", g_cfg.exportNicDrivers ? L"1" : L"0");
    WriteIniStr(L"ImportPatchDriveFix", g_cfg.importPatchDriveFix ? L"1" : L"0");
    WriteIniStr(L"PatchedWimPath", g_cfg.patchedWimPath);
    WriteIniStr(L"PatchedWimSig", g_cfg.patchedWimSig);
    WriteIniStr(L"SdiGuid", g_cfg.sdiGuid);
    WriteIniStr(L"PeGuid", g_cfg.peGuid);

    if (logSaved) {
        AppendLog(L"설정이 저장되었습니다: " + g_iniPath);
    }
    return true;
}

void ScanDirForExtension(const std::wstring& dir, const std::wstring& prefix,
                         const std::wstring& extLower, const std::wstring& preferredNameLower,
                         std::vector<std::wstring>& outItems) {
    std::wstring pattern = JoinPath(dir, L"*" + extLower);
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> others;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        std::wstring rel  = prefix.empty() ? name : (prefix + L"\\" + name);
        if (ToLower(name) == preferredNameLower) {
            outItems.insert(outItems.begin(), rel);
        } else {
            others.push_back(rel);
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    std::sort(others.begin(), others.end());
    outItems.insert(outItems.end(), others.begin(), others.end());
}

std::wstring GetAppDirDriveColon() {
    if (g_appDir.size() >= 2 && std::iswalpha(g_appDir[0]) && g_appDir[1] == L':') {
        std::wstring d = g_appDir.substr(0, 2);
        d[0] = static_cast<wchar_t>(std::towupper(d[0]));
        return d;
    }
    return L"";
}

void UpdateNetworkStatusBanner(const ResolvedFileInfo& wimInfo) {
    if (!g_hLblNetStatus) return;
    net_sync::DetectedNetworkSummary net = net_sync::DetectActiveWindowsNetwork();

    std::wstring txt = (net.foundAny ? L"✔ 연결됨: " : L"⚠ ") + net.shortBannerText + L"  |  WIM 패치: ";
    if (!wimInfo.exists) {
        txt += L"WIM 미선택";
    } else if (IsWimCurrentlyPatched(wimInfo)) {
        txt += L"✔ 적용 완료 (C: 자동복구 + 무인부팅)";
    } else {
        txt += L"[미적용 — C: 자동복구/무인연결 시 패치 필요]";
    }

    SetWindowTextW(g_hLblNetStatus, txt.c_str());
}

void UpdateFileStatusBanner() {
    std::wstring wimTxt = Trim(GetWndText(g_hCmbWim));
    std::wstring sdiTxt = Trim(GetWndText(g_hCmbSdi));

    ResolvedFileInfo wimInfo = ResolvePeFile(wimTxt);
    ResolvedFileInfo sdiInfo = ResolvePeFile(sdiTxt);

    std::wstring status;
    if (wimInfo.exists) {
        wchar_t szBuf[64];
        swprintf_s(szBuf, L"%.1f MB", static_cast<double>(wimInfo.sizeBytes) / (1024.0 * 1024.0));
        net_sync::WimInspectionInfo wi = net_sync::InspectWimFile(wimInfo.fullPath);
        if (!wi.validWimMagic) {
            status += L"[⚠ 손상/비정상 WIM 헤더!]";
        } else if (wi.isLzmsEsd) {
            status += L"[⚠ ESD(LZMS) 압축: " + std::wstring(szBuf) + L" - 표준WIM 변환 필요]";
        } else if (g_isUefi && wi.arch == 0) {
            status += L"[⚠ 32비트(x86) WIM: UEFI 부팅 불가!]";
        } else {
            std::wstring archTag = (wi.arch == 9) ? L", x64" : ((wi.arch == 0) ? L", x86" : L"");
            if (wi.osBuild > 0) archTag += L" Build " + std::to_wstring(wi.osBuild);
            if (wi.bootIndex == 0) archTag += L", BootFlag보정예정";
            status += L"[WIM: " + std::wstring(szBuf) + archTag + L"]";
        }
    } else {
        status += L"[WIM 파일 없음 - 같은 폴더에 .wim 파일을 넣어주세요]";
    }

    status += L"  |  ";

    if (sdiInfo.exists) {
        if (!HasValidSdiSignature(sdiInfo.fullPath)) {
            status += L"[⚠ 손상된 SDI 파일 - '내장 SDI 생성' 필요]";
        } else {
            wchar_t szBuf[64];
            swprintf_s(szBuf, L"%.1f MB", static_cast<double>(sdiInfo.sizeBytes) / (1024.0 * 1024.0));
            status += L"[SDI: " + std::wstring(szBuf) + L"]";
        }
    } else {
        status += L"[SDI 파일 없음 - 우측 '내장 SDI 생성' 클릭 가능]";
    }

    std::wstring targetDrive = wimInfo.exists ? wimInfo.driveColon
                             : (sdiInfo.exists ? sdiInfo.driveColon : GetAppDirDriveColon());
    UINT targetDrvType = DRIVE_UNKNOWN;
    STORAGE_BUS_TYPE targetBus = BusTypeUnknown;
    int blState = -1;
    if (!targetDrive.empty()) {
        std::wstring rootSlash = targetDrive + L"\\";
        targetDrvType = GetDriveTypeW(rootSlash.c_str());
        targetBus     = QueryVolumeStorageBusType(targetDrive);
        blState       = QueryVolumeBitLockerState(targetDrive);
        wchar_t fsBuf[64]{};
        GetVolumeInformationW(rootSlash.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fsBuf, _countof(fsBuf));
        std::wstring fsStr = fsBuf;

        status += L"  |  [" + targetDrive + L"] ";
        if (IsExternalUsbOrCardBus(targetBus) || targetDrvType == DRIVE_REMOVABLE) {
            status += L"(⚠ USB/외장 디스크 - 부팅 시 내장 캐시 자동 복사 지원)";
        } else if (!IsBootSupportedFileSystem(fsStr)) {
            status += L"(⚠ " + fsStr + L" 포맷 - 부트매니저 미지원, 내장 캐시 복사 지원)";
        } else if (IsFileBackedVirtualBus(targetBus) || targetDrvType == DRIVE_CDROM) {
            status += L"(⚠ 가상 VHD/ISO 드라이브 - 내장 캐시 복사 지원)";
        } else if (targetDrvType == DRIVE_FIXED) {
            status += L"(내장 고정 디스크" + (fsStr.empty() ? L"" : (L" · " + fsStr)) + L")";
        } else {
            status += L"(⚠ 특수 드라이브)";
        }
    }

    SetWindowTextW(g_hLblFileStatus, status.c_str());

    if (g_hLblSecStatus) {
        g_secureBootState = DetectSecureBootState();
        std::wstring secTxt;
        if (!targetDrive.empty()) {
            if (blState == 1 || blState == 3 || blState == 6) {
                secTxt += L"⚠ [" + targetDrive + L"] BitLocker: " + DescribeBitLockerState(blState);
            } else {
                secTxt += L"✔ [" + targetDrive + L"] BitLocker: " + DescribeBitLockerState(blState);
            }
        } else {
            secTxt += L"대상 볼륨 미지정";
        }

        secTxt += L"  |  Secure Boot: ";
        if (g_secureBootState == 1) {
            secTxt += L"켜짐 (구형/개조 PE는 차단될 수 있음)";
        } else if (g_secureBootState == 0) {
            secTxt += L"꺼짐";
        } else {
            secTxt += L"미지원(Legacy)";
        }
        SetWindowTextW(g_hLblSecStatus, secTxt.c_str());
    }

    UpdateNetworkStatusBanner(wimInfo);
}

void ActionCheckBitLockerAndSecureBoot() {
    UpdateFileStatusBanner();

    std::wstring wimTxt = Trim(GetWndText(g_hCmbWim));
    std::wstring sdiTxt = Trim(GetWndText(g_hCmbSdi));
    ResolvedFileInfo wimInfo = ResolvePeFile(wimTxt);
    ResolvedFileInfo sdiInfo = ResolvePeFile(sdiTxt);

    std::wstring targetDrive = wimInfo.exists ? wimInfo.driveColon
                             : (sdiInfo.exists ? sdiInfo.driveColon : GetAppDirDriveColon());

    AppendLog(L"--- BitLocker 설정 및 Secure Boot 상태 정밀 진단 ---");

    std::wstring report;
    report += L"[1] 시스템 부팅 보안 (Secure Boot) 상태\n";
    if (!g_isUefi) {
        report += L" • 펌웨어 모드: Legacy BIOS (Secure Boot 해당 없음)\n\n";
    } else if (g_secureBootState == 1) {
        report += L" • 펌웨어 모드: UEFI  |  Secure Boot: 켜짐 (Enabled)\n";
        report += L" • 호환성 안내: 정품 Microsoft 서명이 유지된 최신 Windows 11 기반 WinPE는 Secure Boot가 켜진 상태에서도 정상 부팅됩니다.\n";
        report += L"   (단, 최신 보안 업데이트 KB5025885 등이 적용된 노트북에서는 구형 Win10 PE나 개조 PE의 winload.efi가 0xc000000f / 0xc0000428로 차단될 수 있으므로, 해당 시 Win11 PE를 사용하거나 Secure Boot를 잠시 꺼주세요.)\n\n";
    } else {
        report += L" • 펌웨어 모드: UEFI  |  Secure Boot: 꺼짐 (Disabled)\n";
        report += L" • 호환성 안내: 서명 제한 없이 모든 UEFI WinPE 부팅이 가능합니다.\n\n";
    }

    report += L"[2] 전체 드라이브 BitLocker 및 버스(내장/USB) 현황\n";
    wchar_t drvStrings[512]{};
    DWORD len = GetLogicalDriveStringsW(_countof(drvStrings) - 1, drvStrings);
    const wchar_t* p = drvStrings;
    while (len > 0 && *p != L'\0') {
        std::wstring root(p);
        UINT dtype = GetDriveTypeW(root.c_str());
        if (dtype == DRIVE_FIXED || dtype == DRIVE_REMOVABLE) {
            std::wstring dColon = root.substr(0, 2);
            wchar_t fsName[64]{};
            wchar_t volLabel[128]{};
            GetVolumeInformationW(root.c_str(), volLabel, _countof(volLabel),
                                  nullptr, nullptr, nullptr, fsName, _countof(fsName));
            STORAGE_BUS_TYPE bus = QueryVolumeStorageBusType(dColon);
            int st = QueryVolumeBitLockerState(dColon);
            std::wstring line = L" • " + dColon + L" ";
            if (volLabel[0] != L'\0') line += L"(" + std::wstring(volLabel) + L", ";
            else line += L"(";
            line += (fsName[0] != L'\0' ? std::wstring(fsName) : L"알수없음");
            if (IsExternalUsbOrCardBus(bus) || dtype == DRIVE_REMOVABLE) {
                line += L", USB/외장) : ";
            } else if (IsFileBackedVirtualBus(bus)) {
                line += L", 가상VHD/ISO) : ";
            } else {
                line += L", 내장) : ";
            }
            line += DescribeBitLockerState(st);
            if (_wcsicmp(dColon.c_str(), targetDrive.c_str()) == 0) {
                line += L"  ◀ [현재 PE 대상 볼륨]";
            }
            report += line + L"\n";
            AppendLog(line);
        }
        p += wcslen(p) + 1;
    }

    int targetBlState = targetDrive.empty() ? -1 : QueryVolumeBitLockerState(targetDrive);
    if (!targetDrive.empty() && g_isAdmin) {
        CmdResult bdeRes = RunSystemTool(L"manage-bde.exe", L"-status " + targetDrive, true);
        report += L"\n[3] 대상 볼륨 (" + targetDrive + L") 상세 진단 결과\n";
        if (targetBlState == 1 || targetBlState == 3) {
            report += L" ⚠ 판정: BitLocker 암호화 및 보호가 [켜져 있음]\n";
            report += L" → 조치: PE 부팅 시 0xc000000f 오류 없이 파일을 읽으려면 '재부팅 전 BitLocker 보호 일시 중지' 옵션을 체크하세요.\n";
        } else if (targetBlState == 5) {
            report += L" ✔ 판정: BitLocker가 설정되어 있으나 현재 [보호 일시 중지됨]\n";
            report += L" → 조치: 추가 해제 없이 바로 1회성 PE 부팅이 가능합니다.\n";
        } else if (targetBlState == 0 || targetBlState == 2 || targetBlState == 8) {
            report += L" ✔ 판정: BitLocker 보호가 [꺼져 있음 / 미설정]\n";
            report += L" → 조치: 별도 조치 없이 바로 1회성 PE 부팅이 가능합니다.\n";
        } else {
            report += L" • 판정: " + DescribeBitLockerState(targetBlState) + L"\n";
        }

        if (!bdeRes.output.empty()) {
            report += L"\n[manage-bde 상세 출력 요약 — 전체 원문은 하단 로그 참조]\n";
            size_t pos = 0;
            int addedLines = 0;
            while (pos < bdeRes.output.size() && addedLines < 10) {
                size_t nl = bdeRes.output.find(L'\n', pos);
                std::wstring ln = Trim(nl == std::wstring::npos
                                       ? bdeRes.output.substr(pos)
                                       : bdeRes.output.substr(pos, nl - pos));
                if (!ln.empty() &&
                    ln.find(L"Copyright") == std::wstring::npos &&
                    ln.find(L"Microsoft") == std::wstring::npos &&
                    ln.find(L"Configuration Tool") == std::wstring::npos &&
                    ln.find(L"구성 도구") == std::wstring::npos) {
                    report += L"   " + ln + L"\n";
                    ++addedLines;
                }
                if (nl == std::wstring::npos) break;
                pos = nl + 1;
            }
        }
    }

    if ((targetBlState == 1 || targetBlState == 3) && SendMessageW(g_hChkBitLocker, BM_GETCHECK, 0, 0) != BST_CHECKED) {
        report += L"\n지금 '재부팅 전 BitLocker 보호 일시 중지' 체크박스를 자동으로 켜드릴까요?";
        int ans = MessageBoxW(g_hMainWnd, report.c_str(),
                              L"BitLocker 및 Secure Boot 상태 확인 결과",
                              MB_ICONWARNING | MB_YESNO);
        if (ans == IDYES) {
            SendMessageW(g_hChkBitLocker, BM_SETCHECK, BST_CHECKED, 0);
            AppendLog(L"'재부팅 전 BitLocker 보호 일시 중지' 옵션이 활성화되었습니다.");
        }
    } else {
        MessageBoxW(g_hMainWnd, report.c_str(),
                    L"BitLocker 및 Secure Boot 상태 확인 결과",
                    MB_ICONINFORMATION | MB_OK);
    }
}

void ActionSyncWindowsNetworkNow(bool showMessageBox, const std::wstring& targetDirOverride = L"") {
    PullControlsToConfig();
    std::wstring exportBaseDir = targetDirOverride.empty() ? g_appDir : targetDirOverride;
    std::wstring relAppDir     = GetNormalizedBcdRelativeDir(exportBaseDir);
    std::wstring summary;
    bool ok = net_sync::ExportNetworkConfigForPe(
        exportBaseDir, relAppDir, g_cfg.exportNicDrivers,
        [](const std::wstring& m) { AppendLog(m); },
        summary);

    UpdateFileStatusBanner();
    if (showMessageBox) {
        if (ok) {
            MessageBoxW(g_hMainWnd, summary.c_str(),
                        L"현재 Windows 네트워크 및 드라이버 추출 완료",
                        MB_ICONINFORMATION | MB_OK);
        } else {
            MessageBoxW(g_hMainWnd, L"네트워크 설정 추출 중 오류가 발생했습니다.",
                        L"오류", MB_ICONERROR | MB_OK);
        }
    }
}

void SyncDriveMapAndBridgeForPe(const std::wstring& targetWimPath = L"") {
    PullControlsToConfig();
    std::wstring exportBaseDir = g_appDir;
    if (!targetWimPath.empty()) {
        size_t sl = targetWimPath.find_last_of(L"\\/");
        if (sl != std::wstring::npos) {
            std::wstring wimParent = targetWimPath.substr(0, sl);
            if (ToLower(wimParent).find(L"onetimepe_cache") != std::wstring::npos) {
                exportBaseDir = wimParent;
            }
        }
    }
    std::wstring relAppDir = GetNormalizedBcdRelativeDir(exportBaseDir);
    net_sync::ExportDriveMapForPe(relAppDir, g_cfg.autoApplyNetwork,
                                  [](const std::wstring& m) { AppendLog(m); });
}

void StartAsyncWimPatch(PostPatchFollowUp followUp) {
    if (g_isTaskBusy) return;
    PullControlsToConfig();
    ResolvedFileInfo wimInfo = ResolvePeFile(g_cfg.wimInput);
    if (!wimInfo.exists) {
        MessageBoxW(g_hMainWnd,
                    (L"패치할 WIM 파일을 찾을 수 없습니다:\n" + wimInfo.fullPath).c_str(),
                    L"WIM 파일 없음", MB_ICONWARNING | MB_OK);
        return;
    }

    if (followUp == PostPatchFollowUp::None) {
        std::wstring ask =
            L"선택한 WIM 이미지에 [기존 드라이브 문자(C: 등) 자동 복구 + 무인 부팅 패치]를 적용하시겠습니까?\n\n"
            L"• 대상 파일: " + wimInfo.fullPath + L"\n"
            L"• 수행 작업:\n"
            L"   1) PE 부팅 시 숨김/문자충돌/오프라인 속성을 자동 해제하고 원래 Windows 파티션을 C: 로 고정 복원\n"
            L"   2) startnet.cmd / winpeshl.ini / PECMD.INI / Startup 부팅 훅 + 부트 로더 경로 보정\n"
            L"   3) 'PE 부팅 시 현재 네트워크 자동 적용' 체크 시 드라이버·IP·Wi-Fi 자동 동기화 지원\n\n"
            L"계속하시겠습니까? (약 20~60초 소요)";
        if (MessageBoxW(g_hMainWnd, ask.c_str(), L"WIM 자동 복구 & 무인 부팅 패치 확인",
                        MB_ICONQUESTION | MB_YESNO) != IDYES) {
            return;
        }
    }

    SyncDriveMapAndBridgeForPe(wimInfo.fullPath);

    SetTaskBusyState(true, L"WIM 패치 진행중...");
    std::wstring appDirCopy  = g_appDir;
    std::wstring wimPathCopy = wimInfo.fullPath;

    std::thread([appDirCopy, wimPathCopy, followUp]() {
        auto* payload = new AsyncWimTaskResult();
        payload->kind     = AsyncWimTaskKind::UnattendedNetPatch;
        payload->wimPath  = wimPathCopy;
        payload->followUp = followUp;
        payload->success  = net_sync::PatchBootWimUnattended(
            appDirCopy, wimPathCopy,
            [](const std::wstring& m) { PostLogFromWorkerThread(m); },
            payload->newSig);

        if (!PostMessageW(g_hMainWnd, WM_APP_TASK_DONE, 0, reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    }).detach();
}

void StartAsyncEsdConvert(const std::wstring& wimFullPath, uint32_t sourceIndex, PostPatchFollowUp followUp) {
    if (g_isTaskBusy) return;
    SetTaskBusyState(true, L"ESD→WIM 변환중...");

    std::thread([wimFullPath, sourceIndex, followUp]() {
        auto* payload = new AsyncWimTaskResult();
        payload->kind     = AsyncWimTaskKind::ConvertEsdToWim;
        payload->wimPath  = wimFullPath;
        payload->followUp = followUp;
        payload->success  = net_sync::ConvertEsdWimToBootableWim(
            wimFullPath, sourceIndex,
            [](const std::wstring& m) { PostLogFromWorkerThread(m); },
            payload->newSig);

        if (!PostMessageW(g_hMainWnd, WM_APP_TASK_DONE, 0, reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    }).detach();
}

void ScanLocalPeFiles(bool preserveCurrentText) {
    std::wstring prevWim = preserveCurrentText ? Trim(GetWndText(g_hCmbWim)) : g_cfg.wimInput;
    std::wstring prevSdi = preserveCurrentText ? Trim(GetWndText(g_hCmbSdi)) : g_cfg.sdiInput;

    std::vector<std::wstring> wimList;
    ScanDirForExtension(g_appDir, L"", L".wim", L"boot.wim", wimList);
    ScanDirForExtension(JoinPath(g_appDir, L"sources"), L"sources", L".wim", L"boot.wim", wimList);

    std::vector<std::wstring> sdiList;
    ScanDirForExtension(g_appDir, L"", L".sdi", L"boot.sdi", sdiList);
    ScanDirForExtension(JoinPath(g_appDir, L"boot"), L"boot", L".sdi", L"boot.sdi", sdiList);

    SendMessageW(g_hCmbWim, CB_RESETCONTENT, 0, 0);
    for (const auto& item : wimList) {
        SendMessageW(g_hCmbWim, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    }
    if (wimList.empty()) {
        SendMessageW(g_hCmbWim, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"boot.wim"));
    }

    SendMessageW(g_hCmbSdi, CB_RESETCONTENT, 0, 0);
    for (const auto& item : sdiList) {
        SendMessageW(g_hCmbSdi, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    }
    if (sdiList.empty()) {
        SendMessageW(g_hCmbSdi, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"boot.sdi"));
    }

    ResolvedFileInfo prevWimInfo = ResolvePeFile(prevWim);
    if (!prevWimInfo.exists && !wimList.empty()) {
        SetWindowTextW(g_hCmbWim, wimList.front().c_str());
    } else {
        SetWindowTextW(g_hCmbWim, prevWim.empty() ? L"boot.wim" : prevWim.c_str());
    }

    ResolvedFileInfo prevSdiInfo = ResolvePeFile(prevSdi);
    if (!prevSdiInfo.exists && !sdiList.empty()) {
        SetWindowTextW(g_hCmbSdi, sdiList.front().c_str());
    } else {
        SetWindowTextW(g_hCmbSdi, prevSdi.empty() ? L"boot.sdi" : prevSdi.c_str());
    }

    UpdateFileStatusBanner();
}

void RefreshBcdStatusUI() {
    if (!g_isAdmin) {
        SetWindowTextW(g_hLblBcdStatus, L"BCD 상태: 관리자 권한이 필요합니다.");
        return;
    }

    bool sdiAlive = DoesBcdGuidExist(g_cfg.sdiGuid);
    bool peAlive  = DoesBcdGuidExist(g_cfg.peGuid);
    bool queued   = peAlive && IsGuidInBootSequence(g_cfg.peGuid);

    std::wstring txt = L"BCD 등록 상태: ";
    if (peAlive && sdiAlive) {
        txt += L"등록됨 " + g_cfg.peGuid;
    } else if (peAlive || sdiAlive) {
        txt += L"부분 등록됨 (갱신 필요)";
    } else {
        txt += L"미등록";
    }

    txt += L"   |   1회성 부팅 예약: ";
    if (queued) {
        txt += L"★ 예약됨 (다음 재부팅 시 PE 진입)";
    } else {
        txt += L"없음 (평소대로 기존 Windows 부팅)";
    }

    SetWindowTextW(g_hLblBcdStatus, txt.c_str());
}

void CopyBuiltInBootSdi() {
    wchar_t winDir[MAX_PATH]{};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring wdir(winDir);

    const std::vector<std::wstring> candidates = {
        JoinPath(wdir, L"Boot\\DVD\\EFI\\boot.sdi"),
        JoinPath(wdir, L"Boot\\DVD\\PCAT\\boot.sdi"),
        JoinPath(wdir, L"System32\\Recovery\\boot.sdi")
    };

    std::wstring foundSrc;
    for (const auto& c : candidates) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
            foundSrc = c;
            break;
        }
    }

    if (foundSrc.empty()) {
        MessageBoxW(g_hMainWnd,
                    L"Windows 내장 boot.sdi 파일을 찾을 수 없습니다.\nPE ISO 내의 \\boot\\boot.sdi 파일을 직접 폴더에 넣어주세요.",
                    L"내장 SDI 찾기 실패", MB_ICONWARNING | MB_OK);
        return;
    }

    std::wstring dst = JoinPath(g_appDir, L"boot.sdi");
    if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
        int ans = MessageBoxW(g_hMainWnd,
                              L"현재 폴더에 이미 boot.sdi 파일이 존재합니다. 덮어쓰시겠습니까?",
                              L"파일 덮어쓰기 확인", MB_ICONQUESTION | MB_YESNO);
        if (ans != IDYES) return;
    }

    if (CopyFileW(foundSrc.c_str(), dst.c_str(), FALSE)) {
        AppendLog(L"시스템 내장 boot.sdi 복사 완료: " + foundSrc + L" -> " + dst);
        ScanLocalPeFiles(true);
        SetWindowTextW(g_hCmbSdi, L"boot.sdi");
        UpdateFileStatusBanner();
    } else {
        DWORD err = GetLastError();
        wchar_t msg[256];
        swprintf_s(msg, L"boot.sdi 복사에 실패했습니다. (오류 코드: %lu)", err);
        AppendLog(msg);
        MessageBoxW(g_hMainWnd, msg, L"오류", MB_ICONERROR | MB_OK);
    }
}

// Dynamically load comdlg32.dll so minimal WinPE environments never require comdlg32.dll
void BrowseForFile(HWND hTargetCombo, const wchar_t* filter, const wchar_t* title) {
    HMODULE hComDlg = LoadLibraryW(L"comdlg32.dll");
    if (!hComDlg) return;
    using FnGetOpenFileNameW = BOOL(WINAPI*)(LPOPENFILENAMEW);
    auto pOpenDlg = reinterpret_cast<FnGetOpenFileNameW>(
        reinterpret_cast<void*>(GetProcAddress(hComDlg, "GetOpenFileNameW")));
    if (pOpenDlg) {
        wchar_t fileBuf[MAX_PATH]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize     = sizeof(ofn);
        ofn.hwndOwner       = g_hMainWnd;
        ofn.lpstrFilter     = filter;
        ofn.lpstrFile       = fileBuf;
        ofn.nMaxFile        = _countof(fileBuf);
        ofn.lpstrInitialDir = g_appDir.c_str();
        ofn.lpstrTitle      = title;
        ofn.Flags           = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

        if (pOpenDlg(&ofn)) {
            std::wstring chosen = MakeAppRelativeIfInside(fileBuf);
            SetWindowTextW(hTargetCombo, chosen.c_str());
            UpdateFileStatusBanner();
        }
    }
    FreeLibrary(hComDlg);
}

void RemoveDirectoryTree(const std::wstring& dirPath) {
    DWORD attr = GetFileAttributesW(dirPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(JoinPath(dirPath, L"*").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring full = JoinPath(dirPath, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
                SetFileAttributesW(full.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryTree(full);
            } else {
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    RemoveDirectoryW(dirPath.c_str());
}

std::wstring FileNameFromPath(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    size_t sl = p.find_last_of(L"\\/");
    return (sl == std::wstring::npos) ? p : p.substr(sl + 1);
}

std::wstring StemFromFileName(const std::wstring& fileName) {
    size_t dot = fileName.find_last_of(L'.');
    return (dot == std::wstring::npos || dot == 0) ? fileName : fileName.substr(0, dot);
}

std::wstring SanitizeFileStem(const std::wstring& raw) {
    std::wstring out;
    for (wchar_t c : raw) {
        if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'/' || c == L'\\' || c == L'|' || c == L'?' || c == L'*') {
            out.push_back(L'_');
        } else {
            out.push_back(c);
        }
    }
    out = Trim(out);
    while (!out.empty() && out.back() == L'.') out.pop_back();
    return out.empty() ? L"CustomPE" : out;
}

bool BrowseForCustomPeFile(std::wstring& outFile) {
    outFile.clear();
    HMODULE hComDlg = LoadLibraryW(L"comdlg32.dll");
    if (!hComDlg) return false;
    using FnGetOpenFileNameW = BOOL(WINAPI*)(LPOPENFILENAMEW);
    auto pOpenDlg = reinterpret_cast<FnGetOpenFileNameW>(
        reinterpret_cast<void*>(GetProcAddress(hComDlg, "GetOpenFileNameW")));
    bool picked = false;
    if (pOpenDlg) {
        wchar_t fileBuf[MAX_PATH * 2]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize     = sizeof(ofn);
        ofn.hwndOwner       = g_hMainWnd;
        ofn.lpstrFilter     =
            L"커스텀 PE 이미지 / BCD / WIM (*.iso;BCD;*.bcd;*.wim)\0*.iso;BCD;*.bcd;*.wim\0"
            L"ISO 디스크 이미지 (*.iso)\0*.iso\0"
            L"BCD 부팅 구성 파일 (BCD;*.bcd)\0BCD;*.bcd\0"
            L"WIM 이미지 파일 (*.wim)\0*.wim\0"
            L"모든 파일 (*.*)\0*.*\0";
        ofn.lpstrFile       = fileBuf;
        ofn.nMaxFile        = _countof(fileBuf);
        ofn.lpstrInitialDir = g_appDir.c_str();
        ofn.lpstrTitle      = L"커스텀 PE .ISO 이미지 또는 BCD 파일 선택";
        ofn.Flags           = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (pOpenDlg(&ofn) && fileBuf[0] != L'\0') {
            outFile = fileBuf;
            picked  = true;
        }
    }
    FreeLibrary(hComDlg);
    return picked;
}

bool BrowseForCustomPeFolder(std::wstring& outFolder) {
    outFolder.clear();
    IFileOpenDialog* pDlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&pDlg));
    if (SUCCEEDED(hr) && pDlg) {
        DWORD opts = 0;
        if (SUCCEEDED(pDlg->GetOptions(&opts))) {
            pDlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        }
        pDlg->SetTitle(L"커스텀 PE 루트 폴더 선택 (EFI / boot / sources 등이 들어있는 폴더)");
        if (SUCCEEDED(pDlg->Show(g_hMainWnd))) {
            IShellItem* pItem = nullptr;
            if (SUCCEEDED(pDlg->GetResult(&pItem)) && pItem) {
                PWSTR pszPath = nullptr;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath) {
                    outFolder = pszPath;
                    CoTaskMemFree(pszPath);
                }
                pItem->Release();
            }
        }
        pDlg->Release();
        return !outFolder.empty();
    }

    // Fallback: SHBrowseForFolderW
    BROWSEINFOW bi{};
    bi.hwndOwner = g_hMainWnd;
    bi.lpszTitle = L"커스텀 PE 루트 폴더를 선택하세요:";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t pathBuf[MAX_PATH * 2]{};
        if (SHGetPathFromIDListW(pidl, pathBuf) && pathBuf[0] != L'\0') {
            outFolder = pathBuf;
        }
        CoTaskMemFree(pidl);
    }
    return !outFolder.empty();
}

struct ImportSourceDlgState {
    HWND hChkFixDrv     = nullptr;
    int  choice         = 0; // 0 = cancel, 1 = ISO/BCD file, 2 = Folder
    bool patchFixDrives = true;
    bool done           = false;
};

LRESULT CALLBACK ImportSourceDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<ImportSourceDlgState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, RGB(246, 248, 250));
            SetTextColor(hdc, RGB(30, 35, 45));
            return reinterpret_cast<LRESULT>(g_hBgBrush);
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (HIWORD(wParam) == BN_CLICKED && st) {
                if (id == IDC_DLG_BTN_PICK_ISO || id == IDOK) {
                    if (st->hChkFixDrv) {
                        st->patchFixDrives = (SendMessageW(st->hChkFixDrv, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    }
                    st->choice = 1;
                    st->done   = true;
                    return 0;
                }
                if (id == IDC_DLG_BTN_PICK_DIR) {
                    if (st->hChkFixDrv) {
                        st->patchFixDrives = (SendMessageW(st->hChkFixDrv, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    }
                    st->choice = 2;
                    st->done   = true;
                    return 0;
                }
                if (id == IDC_DLG_BTN_CANCEL || id == IDCANCEL) {
                    st->choice = 0;
                    st->done   = true;
                    return 0;
                }
            }
            break;
        }
        case WM_CLOSE:
            if (st) {
                st->choice = 0;
                st->done   = true;
            }
            return 0;
    }
    return DefWindowProcW(hDlg, msg, wParam, lParam);
}

int ShowImportSourceKindDialog(bool& ioPatchFixDrives) {
    static bool s_registered = false;
    constexpr wchar_t kDlgClass[] = L"OneTimePE_ImportSourceDlg";
    if (!s_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = ImportSourceDlgProc;
        wc.hInstance     = g_hInst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = g_hBgBrush;
        wc.lpszClassName = kDlgClass;
        if (RegisterClassExW(&wc)) s_registered = true;
    }

    int dlgW = Scale(515);
    int dlgH = Scale(292);
    RECT rcParent{};
    GetWindowRect(g_hMainWnd, &rcParent);
    int posX = rcParent.left + ((rcParent.right - rcParent.left) - dlgW) / 2;
    int posY = rcParent.top + ((rcParent.bottom - rcParent.top) - dlgH) / 2;

    ImportSourceDlgState state{};
    state.patchFixDrives = ioPatchFixDrives;
    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kDlgClass,
                                L"커스텀 PE 원클릭 자동 분석 및 가져오기",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                posX, posY, dlgW, dlgH,
                                g_hMainWnd, nullptr, g_hInst, &state);
    if (!hDlg) return 0;

    RECT rcClient{};
    GetClientRect(hDlg, &rcClient);
    int cw  = rcClient.right - rcClient.left;
    int pad = Scale(14);
    int iw  = cw - pad * 2;

    HWND hDesc = CreateWindowExW(
        0, L"STATIC",
        L"커스텀 PE 내부의 EFI\\Microsoft\\Boot\\BCD를 자동으로 분석하여\n"
        L"전용 .wim + 전용 .sdi + 커스텀 부팅 옵션(nointegritychecks 등)을 세팅합니다.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        pad, Scale(12), iw, Scale(38), hDlg, nullptr, g_hInst, nullptr);

    HWND hBtnIso = CreateWindowExW(
        0, L"BUTTON", L"📀  커스텀 PE .ISO 이미지 (또는 BCD) 파일 선택...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        pad, Scale(54), iw, Scale(36), hDlg,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_BTN_PICK_ISO)), g_hInst, nullptr);

    HWND hBtnDir = CreateWindowExW(
        0, L"BUTTON", L"📁  압축 해제된 커스텀 PE 폴더 / USB 드라이브 선택...",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        pad, Scale(96), iw, Scale(36), hDlg,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_BTN_PICK_DIR)), g_hInst, nullptr);

    state.hChkFixDrv = CreateWindowExW(
        0, L"BUTTON",
        L"PE 부팅 시 기존 드라이브 문자(C: 등) 자동 복구 패치 함께 적용 (권장)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        pad, Scale(142), iw, Scale(22), hDlg,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_CHK_FIX_DRV)), g_hInst, nullptr);
    SendMessageW(state.hChkFixDrv, BM_SETCHECK, ioPatchFixDrives ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND hChkSub = CreateWindowExW(
        0, L"STATIC",
        L"   └ 숨김/오프라인 속성 해제 및 EFI 문자 선점 분리로 C: 미설정(케이스 B) 자동 해결",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        pad, Scale(165), iw, Scale(20), hDlg, nullptr, g_hInst, nullptr);

    HWND hTip = CreateWindowExW(
        0, L"STATIC",
        L"💡 팁: 탐색기에서 .iso 파일이나 폴더를 메인 창으로 바로 드래그 앤 드롭해도 됩니다.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        pad, Scale(198), iw - Scale(92), Scale(34), hDlg, nullptr, g_hInst, nullptr);

    HWND hBtnCancel = CreateWindowExW(
        0, L"BUTTON", L"취소",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        pad + iw - Scale(84), Scale(198), Scale(84), Scale(28), hDlg,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_BTN_CANCEL)), g_hInst, nullptr);

    SendMessageW(hDesc, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);
    SendMessageW(hBtnIso, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);
    SendMessageW(hBtnDir, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);
    SendMessageW(state.hChkFixDrv, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);
    SendMessageW(hChkSub, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);
    SendMessageW(hTip, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);
    SendMessageW(hBtnCancel, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

    EnableWindow(g_hMainWnd, FALSE);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
    SetFocus(hBtnIso);

    MSG msg{};
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(g_hMainWnd, TRUE);
    SetForegroundWindow(g_hMainWnd);
    DestroyWindow(hDlg);
    if (state.choice != 0) {
        ioPatchFixDrives = state.patchFixDrives;
    }
    return state.choice;
}

struct EntrySelectDlgState {
    HWND hList          = nullptr;
    HWND hChkFixDrv     = nullptr;
    int  selected       = -1;
    bool patchFixDrives = true;
    bool done           = false;
};

LRESULT CALLBACK EntrySelectDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<EntrySelectDlgState*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, RGB(246, 248, 250));
            SetTextColor(hdc, RGB(30, 35, 45));
            return reinterpret_cast<LRESULT>(g_hBgBrush);
        }
        case WM_COMMAND: {
            const int id   = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (!st) break;
            if ((id == IDC_DLG_PE_LIST && code == LBN_DBLCLK) ||
                ((id == IDC_DLG_BTN_OK || id == IDOK) && code == BN_CLICKED)) {
                int sel = static_cast<int>(SendMessageW(st->hList, LB_GETCURSEL, 0, 0));
                if (sel != LB_ERR) {
                    if (st->hChkFixDrv) {
                        st->patchFixDrives = (SendMessageW(st->hChkFixDrv, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    }
                    st->selected = sel;
                    st->done     = true;
                }
                return 0;
            }
            if ((id == IDC_DLG_BTN_CANCEL || id == IDCANCEL) && code == BN_CLICKED) {
                st->selected = -1;
                st->done     = true;
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            if (st) {
                st->selected = -1;
                st->done     = true;
            }
            return 0;
    }
    return DefWindowProcW(hDlg, msg, wParam, lParam);
}

int SelectCustomPeEntryDialog(const std::vector<net_sync::CustomPeBootEntry>& entries,
                              bool& ioPatchFixDrives) {
    if (entries.empty()) return -1;
    if (entries.size() == 1) return 0;

    static bool s_registered = false;
    constexpr wchar_t kDlgClass[] = L"OneTimePE_EntrySelectDlg";
    if (!s_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = EntrySelectDlgProc;
        wc.hInstance     = g_hInst;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = g_hBgBrush;
        wc.lpszClassName = kDlgClass;
        if (RegisterClassExW(&wc)) s_registered = true;
    }

    int dlgW = Scale(625);
    int dlgH = Scale(360);
    RECT rcParent{};
    GetWindowRect(g_hMainWnd, &rcParent);
    int posX = rcParent.left + ((rcParent.right - rcParent.left) - dlgW) / 2;
    int posY = rcParent.top + ((rcParent.bottom - rcParent.top) - dlgH) / 2;

    EntrySelectDlgState state{};
    state.patchFixDrives = ioPatchFixDrives;
    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kDlgClass,
                                L"가져올 커스텀 PE 부팅 항목 선택",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                posX, posY, dlgW, dlgH,
                                g_hMainWnd, nullptr, g_hInst, &state);
    if (!hDlg) return -1;

    RECT rcClient{};
    GetClientRect(hDlg, &rcClient);
    int cw  = rcClient.right - rcClient.left;
    int ch  = rcClient.bottom - rcClient.top;
    int pad = Scale(14);
    int iw  = cw - pad * 2;

    HWND hLbl = CreateWindowExW(
        0, L"STATIC",
        L"선택한 커스텀 PE의 BCD에서 여러 개의 부팅 이미지가 발견되었습니다. 가져올 항목을 선택하세요:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        pad, Scale(12), iw, Scale(22), hDlg, nullptr, g_hInst, nullptr);

    state.hList = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        pad, Scale(38), iw, ch - Scale(120), hDlg,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_PE_LIST)), g_hInst, nullptr);

    state.hChkFixDrv = CreateWindowExW(
        0, L"BUTTON",
        L"PE 부팅 시 기존 드라이브 문자(C: 등) 자동 복구 패치 함께 적용 (권장)",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        pad, ch - Scale(74), iw, Scale(22), hDlg,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_CHK_FIX_DRV)), g_hInst, nullptr);
    SendMessageW(state.hChkFixDrv, BM_SETCHECK, ioPatchFixDrives ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND hBtnOk = CreateWindowExW(
        0, L"BUTTON", L"선택 항목 가져오기",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        pad + iw - Scale(232), ch - Scale(42), Scale(136), Scale(30), hDlg,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_BTN_OK)), g_hInst, nullptr);

    HWND hBtnCancel = CreateWindowExW(
        0, L"BUTTON", L"취소",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        pad + iw - Scale(88), ch - Scale(42), Scale(88), Scale(30), hDlg,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DLG_BTN_CANCEL)), g_hInst, nullptr);

    SendMessageW(hLbl, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);
    SendMessageW(state.hList, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);
    SendMessageW(state.hChkFixDrv, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);
    SendMessageW(hBtnOk, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);
    SendMessageW(hBtnCancel, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);

    for (const auto& e : entries) {
        std::wstring row;
        if (e.isDefault) row += L"[기본] ";
        row += e.description + L"  —  " + e.wimRelPath + L" (";
        if (e.wimInfo.arch == 9) row += L"x64";
        else if (e.wimInfo.arch == 0) row += (g_isUefi ? L"32비트 x86 · UEFI 부팅불가" : L"x86");
        else row += L"WIM";
        if (e.wimInfo.osBuild > 0) row += L", Build " + std::to_wstring(e.wimInfo.osBuild);
        if (!e.sdiRelPath.empty()) row += L", SDI: " + e.sdiRelPath;
        row += L")";
        SendMessageW(state.hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
    }
    SendMessageW(state.hList, LB_SETHORIZONTALEXTENT, static_cast<WPARAM>(Scale(850)), 0);
    SendMessageW(state.hList, LB_SETCURSEL, 0, 0);

    EnableWindow(g_hMainWnd, FALSE);
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
    SetFocus(state.hList);

    MSG msg{};
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(g_hMainWnd, TRUE);
    SetForegroundWindow(g_hMainWnd);
    DestroyWindow(hDlg);
    if (state.selected >= 0) {
        ioPatchFixDrives = state.patchFixDrives;
    }
    return state.selected;
}

struct CopyProgressCtx {
    std::wstring labelName;
    ULONGLONG    lastTick = 0;
};

DWORD CALLBACK CopyProgressRoutineUI(LARGE_INTEGER TotalFileSize,
                                     LARGE_INTEGER TotalBytesTransferred,
                                     LARGE_INTEGER StreamSize,
                                     LARGE_INTEGER StreamBytesTransferred,
                                     DWORD dwStreamNumber,
                                     DWORD dwCallbackReason,
                                     HANDLE hSourceFile,
                                     HANDLE hDestinationFile,
                                     LPVOID lpData) {
    (void)StreamSize;
    (void)StreamBytesTransferred;
    (void)dwStreamNumber;
    (void)dwCallbackReason;
    (void)hSourceFile;
    (void)hDestinationFile;

    auto* ctx = static_cast<CopyProgressCtx*>(lpData);
    if (!ctx) return PROGRESS_CONTINUE;

    ULONGLONG now = GetTickCount64();
    if (now - ctx->lastTick >= 180 || TotalBytesTransferred.QuadPart >= TotalFileSize.QuadPart) {
        ctx->lastTick = now;
        if (g_hLblFileStatus && TotalFileSize.QuadPart > 0) {
            double doneMb  = static_cast<double>(TotalBytesTransferred.QuadPart) / (1024.0 * 1024.0);
            double totalMb = static_cast<double>(TotalFileSize.QuadPart) / (1024.0 * 1024.0);
            int pct = static_cast<int>((TotalBytesTransferred.QuadPart * 100LL) / TotalFileSize.QuadPart);
            wchar_t buf[256]{};
            swprintf_s(buf, L"커스텀 PE 파일 복사 중: %s (%d%% — %.1f / %.1f MB)...",
                       ctx->labelName.c_str(), pct, doneMb, totalMb);
            SetWindowTextW(g_hLblFileStatus, buf);
            UpdateWindow(g_hLblFileStatus);
        }
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return PROGRESS_CONTINUE;
}

bool CopyFileWithProgressUI(const std::wstring& srcFull,
                            const std::wstring& dstFull,
                            const std::wstring& displayLabel) {
    if (_wcsicmp(srcFull.c_str(), dstFull.c_str()) == 0) return true;

    std::wstring srcSig = net_sync::ComputeFileSignature(srcFull);
    std::wstring dstSig = net_sync::ComputeFileSignature(dstFull);
    if (!srcSig.empty() && srcSig == dstSig) {
        AppendLog(L"동일한 파일이 이미 준비되어 있어 복사를 건너뜁니다: " + dstFull);
        return true;
    }

    DWORD dstAttr = GetFileAttributesW(dstFull.c_str());
    if (dstAttr != INVALID_FILE_ATTRIBUTES && (dstAttr & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(dstFull.c_str(), dstAttr & ~FILE_ATTRIBUTE_READONLY);
    }

    AppendLog(L"파일 복사 중: " + srcFull + L" -> " + dstFull);
    CopyProgressCtx ctx{};
    ctx.labelName = displayLabel;
    BOOL cancelFlag = FALSE;
    HCURSOR prevCur = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    BOOL ok = CopyFileExW(srcFull.c_str(), dstFull.c_str(),
                          CopyProgressRoutineUI, &ctx, &cancelFlag, 0);
    SetCursor(prevCur);

    if (!ok) {
        DWORD err = GetLastError();
        DeleteFileW(dstFull.c_str());
        wchar_t errMsg[512]{};
        swprintf_s(errMsg,
                   L"커스텀 PE 파일 복사에 실패했습니다. (오류 코드: %lu)\n대상 경로: %s\n디스크 여유 공간 및 권한을 확인해 주세요.",
                   err, dstFull.c_str());
        AppendLog(errMsg);
        MessageBoxW(g_hMainWnd, errMsg, L"커스텀 PE 복사 실패", MB_ICONERROR | MB_OK);
        return false;
    }

    DWORD newAttr = GetFileAttributesW(dstFull.c_str());
    if (newAttr != INVALID_FILE_ATTRIBUTES && (newAttr & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(dstFull.c_str(), newAttr & ~FILE_ATTRIBUTE_READONLY);
    }
    if (newAttr != INVALID_FILE_ATTRIBUTES && (newAttr & FILE_ATTRIBUTE_COMPRESSED)) {
        RunSystemTool(L"compact.exe", L"/u \"" + dstFull + L"\"", false);
    }
    return true;
}

void ImportCustomPeFromPath(const std::wstring& selectedPath, int patchChoiceMode = -1) {
    if (g_isTaskBusy) return;
    std::wstring cleanSel = Trim(selectedPath);
    if (cleanSel.empty()) return;

    DWORD selAttr = GetFileAttributesW(cleanSel.c_str());
    if (selAttr == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_hMainWnd,
                    (L"선택한 파일 또는 폴더를 찾을 수 없습니다:\n" + cleanSel).c_str(),
                    L"경로 오류", MB_ICONWARNING | MB_OK);
        return;
    }

    bool isDir = (selAttr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring lowSel = ToLower(cleanSel);
    bool isIso = !isDir && lowSel.size() >= 4 && lowSel.substr(lowSel.size() - 4) == L".iso";

    net_sync::IsoMountSession isoSession{};
    struct IsoGuard {
        net_sync::IsoMountSession& s;
        ~IsoGuard() {
            net_sync::UnmountIsoSession(s, [](const std::wstring& m) { AppendLog(m); });
        }
    } isoGuard{ isoSession };

    std::wstring analyzeTarget = cleanSel;
    if (isIso) {
        if (!net_sync::MountIsoReadOnly(cleanSel, isoSession, [](const std::wstring& m) { AppendLog(m); })) {
            MessageBoxW(g_hMainWnd,
                        (L"선택한 ISO 이미지를 마운트할 수 없습니다:\n" + cleanSel +
                         L"\n\n탐색기에서 ISO 파일을 우클릭 -> [탑재] 한 뒤 마운트된 드라이브 폴더를 선택해 보세요.").c_str(),
                        L"ISO 마운트 실패", MB_ICONERROR | MB_OK);
            return;
        }
        analyzeTarget = isoSession.driveRoot;
    }

    net_sync::CustomPeDiscoveryResult disc = net_sync::AnalyzeCustomPeMedia(
        analyzeTarget, g_isUefi, [](const std::wstring& m) { AppendLog(m); });
    if (!disc.ok || disc.entries.empty()) {
        MessageBoxW(g_hMainWnd, disc.errorMsg.c_str(), L"커스텀 PE 분석 실패", MB_ICONWARNING | MB_OK);
        return;
    }

    bool applyDriveFix = (patchChoiceMode == 1) ? true
                       : ((patchChoiceMode == 0) ? false : g_cfg.importPatchDriveFix);

    int chosenIdx = SelectCustomPeEntryDialog(disc.entries, applyDriveFix);
    if (chosenIdx < 0 || chosenIdx >= static_cast<int>(disc.entries.size())) {
        AppendLog(L"커스텀 PE 가져오기가 취소되었습니다.");
        return;
    }

    // When imported via Drag & Drop with a single boot entry (no dialog shown yet), let user choose Drive-Fix patch
    if (patchChoiceMode == -1 && disc.entries.size() == 1) {
        UINT defBtn = applyDriveFix ? MB_DEFBUTTON1 : MB_DEFBUTTON2;
        int patchAns = MessageBoxW(
            g_hMainWnd,
            L"가져올 커스텀 PE 이미지에 [기존 드라이브 문자(C: 등) 자동 복구 패치]를 함께 적용하시겠습니까?\n\n"
            L"• [예(Y)] (권장) : 패치 함께 적용\n"
            L"   └ PE 부팅 시 디스크/볼륨 숨김·오프라인 속성 해제 및 EFI 문자 선점 분리로 C: 미설정(케이스 B) 자동 해결\n"
            L"• [아니오(N)] : 패치 미적용 (원본 WIM 100% 보존 상태로 가져오기)\n"
            L"• [취소] : 커스텀 PE 가져오기 취소",
            L"커스텀 PE 가져오기 — C: 드라이브 자동 복구 선택",
            MB_ICONQUESTION | MB_YESNOCANCEL | defBtn);
        if (patchAns == IDCANCEL) {
            AppendLog(L"커스텀 PE 가져오기가 취소되었습니다.");
            return;
        }
        applyDriveFix = (patchAns == IDYES);
    }
    g_cfg.importPatchDriveFix = applyDriveFix;

    const net_sync::CustomPeBootEntry& entry = disc.entries[static_cast<size_t>(chosenIdx)];
    if (g_isUefi && entry.wimInfo.arch == 0) {
        int cont = MessageBoxW(
            g_hMainWnd,
            L"선택한 커스텀 PE 항목은 [32비트(x86) WinPE] 이미지입니다.\n"
            L"현재 PC는 64비트 UEFI 모드이므로 이 이미지로는 부팅할 수 없습니다(0xc000000f).\n\n"
            L"그래도 가져오기를 계속하시겠습니까?",
            L"32비트(x86) 커스텀 PE 경고", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
        if (cont != IDYES) return;
    }

    auto isInsideDirCaseInsensitive = [](const std::wstring& filePath, const std::wstring& dirPath) -> bool {
        if (dirPath.empty() || filePath.size() <= dirPath.size()) return false;
        if (_wcsnicmp(filePath.c_str(), dirPath.c_str(), dirPath.size()) != 0) return false;
        wchar_t lastDirCh = dirPath.back();
        if (lastDirCh == L'\\' || lastDirCh == L'/') return true;
        wchar_t sep = filePath[dirPath.size()];
        return (sep == L'\\' || sep == L'/');
    };
    std::wstring cacheCheckDir = GetSystemDriveColon() + L"\\OneTimePE_Cache";
    bool alreadyInManagedDir = isInsideDirCaseInsensitive(entry.wimFullPath, g_appDir) ||
                               isInsideDirCaseInsensitive(entry.wimFullPath, cacheCheckDir);

    // Decide whether to copy WIM + SDI to local internal storage or link in-place.
    // When applyDriveFix is enabled, always stage external unpacked folders into g_appDir / Cache so user's original WIM stays untouched.
    ResolvedFileInfo srcWimInfo = ResolvePeFile(entry.wimFullPath);
    bool mustCopy = isIso ||
                    (applyDriveFix && !alreadyInManagedDir) ||
                    (srcWimInfo.driveType != DRIVE_FIXED) ||
                    IsExternalUsbOrCardBus(srcWimInfo.busType) ||
                    IsFileBackedVirtualBus(srcWimInfo.busType) ||
                    !IsBootSupportedFileSystem(srcWimInfo.fileSystem);

    std::wstring wimStem = StemFromFileName(FileNameFromPath(entry.wimFullPath));
    std::wstring pairStem;
    if (_wcsicmp(wimStem.c_str(), L"boot") != 0 && !wimStem.empty()) {
        pairStem = SanitizeFileStem(wimStem);
    } else if (isIso) {
        pairStem = SanitizeFileStem(StemFromFileName(FileNameFromPath(cleanSel)));
    } else {
        std::wstring rootName = FileNameFromPath(disc.mediaRoot);
        if (rootName.size() > 3 && _wcsicmp(rootName.c_str(), L"boot") != 0) {
            pairStem = SanitizeFileStem(rootName);
        } else {
            pairStem = L"custom_pe";
        }
    }
    if (_wcsicmp(pairStem.c_str(), L"boot") == 0) {
        pairStem = L"custom_pe";
    }

    std::wstring finalWimPath = entry.wimFullPath;
    std::wstring finalSdiPath = entry.sdiFullPath;

    if (mustCopy) {
        // Choose destination: g_appDir if it sits on an internal fixed NTFS/FAT32 volume, else C:\OneTimePE_Cache
        std::wstring localTargetDir = g_appDir;
        std::wstring appDrv = GetAppDirDriveColon();
        bool appDirBootSafe = false;
        if (!appDrv.empty()) {
            std::wstring rootSlash = appDrv + L"\\";
            UINT dt = GetDriveTypeW(rootSlash.c_str());
            STORAGE_BUS_TYPE bt = QueryVolumeStorageBusType(appDrv);
            wchar_t fsBuf[64]{};
            GetVolumeInformationW(rootSlash.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fsBuf, _countof(fsBuf));
            appDirBootSafe = (dt == DRIVE_FIXED) && !IsExternalUsbOrCardBus(bt) &&
                             !IsFileBackedVirtualBus(bt) && IsBootSupportedFileSystem(fsBuf);
        }
        if (!appDirBootSafe) {
            localTargetDir = GetSystemDriveColon() + L"\\OneTimePE_Cache";
            CreateDirectoryW(localTargetDir.c_str(), nullptr);
            DWORD ca = GetFileAttributesW(localTargetDir.c_str());
            if (ca != INVALID_FILE_ATTRIBUTES && (ca & FILE_ATTRIBUTE_COMPRESSED)) {
                RunSystemTool(L"compact.exe", L"/u /s:\"" + localTargetDir + L"\"", false);
            }
        }

        std::wstring dstWim = JoinPath(localTargetDir, pairStem + L".wim");
        std::wstring dstSdi = JoinPath(localTargetDir, pairStem + L".sdi");

        SetTaskBusyState(true, L"커스텀PE 복사중...");
        bool copyOk = true;
        if (!entry.sdiFullPath.empty() && GetFileAttributesW(entry.sdiFullPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            copyOk = CopyFileWithProgressUI(entry.sdiFullPath, dstSdi, pairStem + L".sdi");
        } else {
            AppendLog(L"참고: 커스텀 PE 미디어 내에 별도 .sdi 파일이 없어 Windows 내장 SDI로 대체 생성합니다.");
            CopyBuiltInBootSdi();
            dstSdi = JoinPath(g_appDir, L"boot.sdi");
        }
        if (copyOk) {
            copyOk = CopyFileWithProgressUI(entry.wimFullPath, dstWim, pairStem + L".wim");
        }
        SetTaskBusyState(false);

        if (!copyOk) {
            UpdateFileStatusBanner();
            return;
        }
        finalWimPath = dstWim;
        finalSdiPath = dstSdi;

        // Unmount ISO immediately now that files are safely staged locally
        net_sync::UnmountIsoSession(isoSession, [](const std::wstring& m) { AppendLog(m); });
    } else {
        if (finalSdiPath.empty() || GetFileAttributesW(finalSdiPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            CopyBuiltInBootSdi();
            finalSdiPath = JoinPath(g_appDir, L"boot.sdi");
        }
        for (const std::wstring& fp : { finalWimPath, finalSdiPath }) {
            DWORD fa = GetFileAttributesW(fp.c_str());
            if (fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_READONLY)) {
                SetFileAttributesW(fp.c_str(), fa & ~FILE_ATTRIBUTE_READONLY);
            }
            if (fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_COMPRESSED)) {
                RunSystemTool(L"compact.exe", L"/u \"" + fp + L"\"", false);
            }
        }
        AppendLog(L"내장 고정 디스크 경로의 커스텀 PE 파일을 복사 없이 직접 연결했습니다.");
    }

    // Populate UI controls from imported Custom PE entry
    ScanLocalPeFiles(true);
    SetWindowTextW(g_hCmbWim, MakeAppRelativeIfInside(finalWimPath).c_str());
    SetWindowTextW(g_hCmbSdi, MakeAppRelativeIfInside(finalSdiPath).c_str());

    std::wstring desc = !entry.description.empty() ? entry.description : (pairStem + L" PE");
    SetWindowTextW(g_hEdtDesc, desc.c_str());

    std::wstring sysRoot = !entry.wimInfo.systemRoot.empty()
                           ? entry.wimInfo.systemRoot
                           : (!entry.systemRoot.empty() ? entry.systemRoot : L"\\windows");
    SetWindowTextW(g_hEdtSysRoot, sysRoot.c_str());

    std::wstring ldr = NormalizeLoaderPathForFirmware(entry.loaderPath, g_isUefi);
    uint32_t probeIdx = (entry.wimInfo.bootIndex > 0)
                        ? entry.wimInfo.bootIndex
                        : ((entry.wimInfo.imageCount > 0) ? entry.wimInfo.imageCount : 1u);
    std::wstring verifiedLdr = net_sync::DetectPreferredLoaderPathInWim(finalWimPath, probeIdx, g_isUefi, ldr);
    if (!verifiedLdr.empty()) {
        ldr = verifiedLdr;
    }
    SetWindowTextW(g_hCmbLoader, ldr.c_str());

    bool useWinpe = entry.hasWinpeFlag ? entry.winpeMode : true;
    bool useHal   = entry.hasDetectHalFlag ? entry.detectHal : true;
    SendMessageW(g_hChkWinpe, BM_SETCHECK, useWinpe ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_hChkDetectHal, BM_SETCHECK, useHal ? BST_CHECKED : BST_UNCHECKED, 0);

    int nxSel = 0;
    if (_wcsicmp(entry.nxPolicy.c_str(), L"OptIn") == 0) nxSel = 1;
    else if (_wcsicmp(entry.nxPolicy.c_str(), L"OptOut") == 0) nxSel = 2;
    else if (_wcsicmp(entry.nxPolicy.c_str(), L"AlwaysOff") == 0) nxSel = 3;
    else if (_wcsicmp(entry.nxPolicy.c_str(), L"AlwaysOn") == 0) nxSel = 4;
    SendMessageW(g_hCmbNx, CB_SETCURSEL, nxSel, 0);

    // Ensure custom PE signature bypass option (nointegritychecks yes) is included in ExtraSetArgs
    std::wstring extra = entry.extraSetArgs;
    if (ToLower(extra).find(L"nointegritychecks") == std::wstring::npos) {
        extra = extra.empty() ? L"nointegritychecks yes" : (L"nointegritychecks yes; " + extra);
    }
    SetWindowTextW(g_hEdtExtra, extra.c_str());

    // Keep auto-network unchecked by default on Custom PE unless user explicitly turns it on
    SendMessageW(g_hChkAutoNet, BM_SETCHECK, BST_UNCHECKED, 0);

    bool alreadyPatched = net_sync::IsWimUnattendedPatched(finalWimPath, probeIdx);
    if (alreadyPatched) {
        g_cfg.patchedWimPath = finalWimPath;
        g_cfg.patchedWimSig  = net_sync::ComputeFileSignature(finalWimPath);
    }

    SaveConfigToIni(false);
    SyncDriveMapAndBridgeForPe(finalWimPath);
    UpdateFileStatusBanner();

    AppendLog(L"★ 커스텀 PE 원클릭 세팅 완료: [" + desc + L"] WIM=" + finalWimPath + L", SDI=" + finalSdiPath);

    std::wstring summary = L"커스텀 PE 분석 및 원클릭 부팅 세팅이 완료되었습니다!\n\n";
    summary += L"• 부팅 항목명 : " + desc + L"\n";
    summary += L"• 분석된 구성 : " + (disc.synthesizedWithoutBcd ? L"(폴더 내 WIM/SDI 직접 매칭)" : disc.usedBcdPath) + L"\n";
    summary += L"• 전용 WIM    : " + MakeAppRelativeIfInside(finalWimPath) + L"\n";
    summary += L"• 전용 SDI    : " + MakeAppRelativeIfInside(finalSdiPath) + L" (순정 SDI와 충돌 방지됨)\n";
    summary += L"• 부트 로더   : " + ldr + L"\n";
    summary += L"• 커스텀 옵션 : " + extra + L"\n";
    if (applyDriveFix) {
        summary += L"• C: 자동복구 : 활성화됨 (PE 부팅 시 숨김/오프라인 해제 및 C: 문자 선점 자동 분리)\n";
        summary += L"• 네트워크    : 'PE 부팅 시 현재 네트워크 자동 적용'은 기본 해제됨 (원할 때 체크 가능)\n";
    } else if (alreadyPatched) {
        summary += L"• C: 자동복구 : 활성화됨 (해당 WIM에 이미 자동복구 패치가 포함되어 있음)\n";
    } else {
        summary += L"• C: 자동복구 : 미적용 (선택 해제됨 — 원본 WIM 100% 무수정 보존)\n";
    }

    if (mustCopy && !disc.externalToolMarkers.empty()) {
        std::wstring mkList;
        for (size_t i = 0; i < disc.externalToolMarkers.size(); ++i) {
            if (i > 0) mkList += L", ";
            mkList += disc.externalToolMarkers[i];
        }
        summary += L"\n💡 외부 도구 폴더 감지 (" + mkList + L"):\n"
                   L"   부팅 자체는 지금 바로 가능하며, PE 부팅 후 외부 포터블 프로그램까지 불러오려면 원본의 해당 폴더를 내장 드라이브 루트(예: C:\\ 또는 D:\\)에 복사해 두세요.\n";
    }

    g_secureBootState = DetectSecureBootState();
    bool warnSb = (g_isUefi && g_secureBootState == 1);
    if (warnSb) {
        summary += L"\n⚠ 중요 — 현재 UEFI Secure Boot가 [켜짐] 상태입니다!\n"
                   L"   개조/구형 커스텀 PE의 winload.efi는 Secure Boot가 켜져 있으면 0xc000000f 또는 0xc0000428 오류로 차단될 수 있습니다.\n"
                   L"   부팅 오류가 발생할 경우 BIOS 설정(삼성 노트북 F2 → Boot → Secure Boot Control → Disabled)에서 Secure Boot를 잠시 꺼주세요.";
    } else {
        summary += L"\n이제 하단의 [▶ 저장 후 지금 PE 재부팅] 또는 [1회성 예약만] 버튼을 누르면 바로 부팅됩니다.";
    }

    if (applyDriveFix && !alreadyPatched) {
        if (!g_isAdmin) {
            AppendLog(L"⚠ 관리자 권한이 없어 WIM 내부 [C: 드라이브 자동 복구 패치]를 생략했습니다.");
            summary += L"\n\n⚠ 관리자 권한이 아니므로 WIM 패치는 적용되지 않았습니다. 관리자 권한으로 실행 후 [WIM 자동복구 패치]를 눌러주세요.";
            MessageBoxW(g_hMainWnd, summary.c_str(), L"커스텀 PE 가져오기 완료 (권한 안내)",
                        MB_ICONWARNING | MB_OK);
            return;
        }

        AppendLog(L"선택한 옵션에 따라 커스텀 PE WIM에 [기존 드라이브 문자(C: 등) 자동 복구 패치]를 적용합니다...");
        SetTaskBusyState(true, L"C: 자동복구 패치중...");
        std::wstring appDirCopy  = g_appDir;
        std::wstring wimPathCopy = finalWimPath;

        std::thread([appDirCopy, wimPathCopy, summary, warnSb]() {
            auto* payload = new AsyncWimTaskResult();
            payload->kind           = AsyncWimTaskKind::CustomImportDriveFixPatch;
            payload->wimPath        = wimPathCopy;
            payload->followUp       = PostPatchFollowUp::None;
            payload->customSummary  = summary;
            payload->warnSecureBoot = warnSb;
            payload->success        = net_sync::PatchBootWimUnattended(
                appDirCopy, wimPathCopy,
                [](const std::wstring& m) { PostLogFromWorkerThread(m); },
                payload->newSig);
            if (!PostMessageW(g_hMainWnd, WM_APP_TASK_DONE, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        }).detach();
        return;
    }

    if (warnSb) {
        MessageBoxW(g_hMainWnd, summary.c_str(), L"커스텀 PE 가져오기 완료 (Secure Boot 안내 포함)",
                    MB_ICONWARNING | MB_OK);
    } else {
        MessageBoxW(g_hMainWnd, summary.c_str(), L"커스텀 PE 가져오기 완료",
                    MB_ICONINFORMATION | MB_OK);
    }
}

void ActionImportCustomPe() {
    if (g_isTaskBusy) return;
    bool patchFix = g_cfg.importPatchDriveFix;
    int kind = ShowImportSourceKindDialog(patchFix);
    if (kind == 1 || kind == 2) {
        g_cfg.importPatchDriveFix = patchFix;
        SaveConfigToIni(false);
    }
    if (kind == 1) {
        std::wstring filePath;
        if (BrowseForCustomPeFile(filePath)) {
            ImportCustomPeFromPath(filePath, patchFix ? 1 : 0);
        }
    } else if (kind == 2) {
        std::wstring folderPath;
        if (BrowseForCustomPeFolder(folderPath)) {
            ImportCustomPeFromPath(folderPath, patchFix ? 1 : 0);
        }
    }
}

bool StageFilesToInternalCache(ResolvedFileInfo& ioWim, ResolvedFileInfo& ioSdi) {
    std::wstring sysDrv = GetSystemDriveColon();
    if (sysDrv.empty()) return false;
    std::wstring cacheDir = sysDrv + L"\\OneTimePE_Cache";
    CreateDirectoryW(cacheDir.c_str(), nullptr);

    DWORD dirAttr = GetFileAttributesW(cacheDir.c_str());
    if (dirAttr != INVALID_FILE_ATTRIBUTES && (dirAttr & FILE_ATTRIBUTE_COMPRESSED)) {
        RunSystemTool(L"compact.exe", L"/u /s:\"" + cacheDir + L"\"", false);
    }

    auto extractFileName = [](const std::wstring& p, const wchar_t* fallback) -> std::wstring {
        size_t pos = p.find_last_of(L"\\/");
        std::wstring name = (pos == std::wstring::npos) ? p : p.substr(pos + 1);
        return name.empty() ? std::wstring(fallback) : name;
    };

    auto stageOne = [&](const std::wstring& srcFull, const wchar_t* fallbackName, std::wstring& outDstFull) -> bool {
        outDstFull = JoinPath(cacheDir, extractFileName(srcFull, fallbackName));
        if (_wcsicmp(srcFull.c_str(), outDstFull.c_str()) == 0) return true;

        std::wstring srcSig = net_sync::ComputeFileSignature(srcFull);
        std::wstring dstSig = net_sync::ComputeFileSignature(outDstFull);
        if (!srcSig.empty() && srcSig == dstSig) {
            AppendLog(L"내장 캐시에 동일한 파일이 이미 준비되어 있어 복사를 건너뜁니다: " + outDstFull);
            return true;
        }

        DWORD dstAttr = GetFileAttributesW(outDstFull.c_str());
        if (dstAttr != INVALID_FILE_ATTRIBUTES && (dstAttr & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesW(outDstFull.c_str(), dstAttr & ~FILE_ATTRIBUTE_READONLY);
        }

        AppendLog(L"내장 시스템 드라이브로 부팅 이미지 복사 중 (잠시만 기다려 주세요): " + srcFull + L" -> " + outDstFull);
        UpdateWindow(g_hEdtLog);
        HCURSOR prevCur = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        BOOL copied = CopyFileW(srcFull.c_str(), outDstFull.c_str(), FALSE);
        SetCursor(prevCur);
        if (!copied) {
            DWORD err = GetLastError();
            wchar_t errMsg[512];
            swprintf_s(errMsg,
                       L"내장 드라이브(%s)로 파일 복사에 실패했습니다. (오류 코드: %lu)\n시스템 드라이브 여유 공간을 확인해 주세요.",
                       outDstFull.c_str(), err);
            AppendLog(errMsg);
            MessageBoxW(g_hMainWnd, errMsg, L"내장 캐시 복사 실패", MB_ICONERROR | MB_OK);
            return false;
        }

        DWORD newAttr = GetFileAttributesW(outDstFull.c_str());
        if (newAttr != INVALID_FILE_ATTRIBUTES && (newAttr & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesW(outDstFull.c_str(), newAttr & ~FILE_ATTRIBUTE_READONLY);
        }
        if (newAttr != INVALID_FILE_ATTRIBUTES && (newAttr & FILE_ATTRIBUTE_COMPRESSED)) {
            RunSystemTool(L"compact.exe", L"/u \"" + outDstFull + L"\"", false);
        }
        return true;
    };

    std::wstring dstSdi, dstWim;
    if (!stageOne(ioSdi.fullPath, L"boot.sdi", dstSdi)) return false;
    if (!stageOne(ioWim.fullPath, L"boot.wim", dstWim)) return false;

    SetWindowTextW(g_hCmbSdi, dstSdi.c_str());
    SetWindowTextW(g_hCmbWim, dstWim.c_str());
    PullControlsToConfig();

    ioSdi = ResolvePeFile(g_cfg.sdiInput);
    ioWim = ResolvePeFile(g_cfg.wimInput);
    UpdateFileStatusBanner();
    return ioSdi.exists && ioWim.exists;
}

bool ValidateInputsAndFiles(ResolvedFileInfo& outWim,
                            ResolvedFileInfo& outSdi,
                            PostPatchFollowUp esdFollowUp = PostPatchFollowUp::None) {
    PullControlsToConfig();

    // 1. Auto-normalize loader path for current firmware (UEFI .efi vs Legacy BIOS .exe)
    std::wstring normLoader = NormalizeLoaderPathForFirmware(g_cfg.loaderPath, g_isUefi);
    if (_wcsicmp(normLoader.c_str(), g_cfg.loaderPath.c_str()) != 0) {
        AppendLog(L"현재 펌웨어 모드(" + std::wstring(g_isUefi ? L"UEFI" : L"Legacy BIOS") +
                  L")에 맞춰 부트 로더 경로 자동 보정: " + g_cfg.loaderPath + L" -> " + normLoader);
        g_cfg.loaderPath = normLoader;
        SetWindowTextW(g_hCmbLoader, g_cfg.loaderPath.c_str());
    }

    outWim = ResolvePeFile(g_cfg.wimInput);
    outSdi = ResolvePeFile(g_cfg.sdiInput);

    if (!outWim.exists) {
        std::wstring msg = L"지정된 WIM 파일을 찾을 수 없습니다:\n" + outWim.fullPath +
                           L"\n\n같은 폴더에 PE용 .wim 파일(예: boot.wim)을 넣었는지 확인하세요.";
        MessageBoxW(g_hMainWnd, msg.c_str(), L"WIM 파일 없음", MB_ICONWARNING | MB_OK);
        return false;
    }
    if (!outSdi.exists) {
        int ans = MessageBoxW(g_hMainWnd,
                              L"지정된 boot.sdi 파일을 찾을 수 없습니다.\n지금 Windows 내장 boot.sdi 파일을 현재 폴더에 자동으로 생성할까요?",
                              L"SDI 파일 없음", MB_ICONQUESTION | MB_YESNO);
        if (ans == IDYES) {
            CopyBuiltInBootSdi();
            PullControlsToConfig();
            outSdi = ResolvePeFile(g_cfg.sdiInput);
        }
        if (!outSdi.exists) return false;
    }

    if (!HasValidSdiSignature(outSdi.fullPath)) {
        int ans = MessageBoxW(
            g_hMainWnd,
            (L"선택한 SDI 파일이 손상되었거나 올바른 RAMDisk SDI 포맷($SDI0001)이 아닙니다:\n" + outSdi.fullPath +
             L"\n\n손상된 SDI 파일은 부팅 시 0xc000000f 오류를 유발합니다.\n지금 정상 Windows 내장 boot.sdi로 교체할까요?").c_str(),
            L"손상된 boot.sdi 감지", MB_ICONWARNING | MB_YESNO);
        if (ans == IDYES) {
            CopyBuiltInBootSdi();
            PullControlsToConfig();
            outSdi = ResolvePeFile(g_cfg.sdiInput);
        }
        if (!outSdi.exists || !HasValidSdiSignature(outSdi.fullPath)) return false;
    }

    if (outWim.driveColon.empty() || outSdi.driveColon.empty()) {
        MessageBoxW(g_hMainWnd,
                    L"네트워크 경로(UNC)에서는 부트 매니저가 파일을 읽을 수 없습니다. 로컬 드라이브에서 실행해 주세요.",
                    L"지원되지 않는 경로", MB_ICONERROR | MB_OK);
        return false;
    }

    if (outWim.isEncryptedEfs || outSdi.isEncryptedEfs) {
        MessageBoxW(g_hMainWnd,
                    L"WIM 또는 SDI 파일에 NTFS 파일 암호화(EFS)가 걸려 있어 부트 매니저가 읽을 수 없습니다.\n파일 속성 -> 고급에서 '내용을 암호화하여 데이터 보호'를 해제해 주세요.",
                    L"EFS 암호화 감지", MB_ICONERROR | MB_OK);
        return false;
    }

    // 2. Check USB / external drive / exFAT / virtual VHD-ISO BEFORE modifying WIM so read-only/USB media can stage cleanly to C:\OneTimePE_Cache
    bool onExternalUsb = IsExternalUsbOrCardBus(outWim.busType) || IsExternalUsbOrCardBus(outSdi.busType) ||
                         (outWim.driveType == DRIVE_REMOVABLE) || (outSdi.driveType == DRIVE_REMOVABLE);
    bool onVirtualDisk = IsFileBackedVirtualBus(outWim.busType) || IsFileBackedVirtualBus(outSdi.busType) ||
                         (outWim.driveType == DRIVE_CDROM) || (outSdi.driveType == DRIVE_CDROM);
    bool onUnsupportedFs = !IsBootSupportedFileSystem(outWim.fileSystem) || !IsBootSupportedFileSystem(outSdi.fileSystem);
    bool nonFixedOther   = (outWim.driveType != DRIVE_FIXED) || (outSdi.driveType != DRIVE_FIXED);

    if (onExternalUsb || onVirtualDisk || onUnsupportedFs || nonFixedOther) {
        std::wstring sysDrv = GetSystemDriveColon();
        STORAGE_BUS_TYPE sysBus = QueryVolumeStorageBusType(sysDrv);
        bool canStageInternal = !sysDrv.empty() && !IsExternalUsbOrCardBus(sysBus);

        std::wstring reason;
        if (onVirtualDisk) {
            reason = L"현재 PE 파일이 [마운트된 가상 드라이브(ISO/VHD)]에 위치해 있습니다.\n재부팅하면 마운트가 해제되어 100% 부팅 오류(0xc000000f)가 발생합니다.";
        } else if (onUnsupportedFs) {
            std::wstring badFs = !IsBootSupportedFileSystem(outWim.fileSystem) ? outWim.fileSystem : outSdi.fileSystem;
            reason = L"현재 PE 파일이 [" + badFs + L" 파일 시스템] 볼륨에 위치해 있습니다.\nWindows 부트 매니저는 NTFS/FAT32만 읽을 수 있어 " + badFs + L" 볼륨에서는 0xc000000f 오류가 발생합니다.";
        } else if (onExternalUsb) {
            reason = L"현재 PE 파일이 [USB 메모리 / 외장 디스크(" + outWim.driveColon + L")]에 위치해 있습니다.\n삼성 노트북 등 UEFI Fast Boot 환경에서는 내장 부트 매니저가 재부팅 초기 단계에 외장 USB 볼륨을 열지 못해 \\windows\\system32\\winload.efi (0xc000000f) 오류가 발생합니다.";
        } else {
            reason = L"현재 PE 파일이 표준 내장 고정 디스크가 아닌 볼륨(" + outWim.driveColon + L")에 위치해 있습니다.";
        }

        if (canStageInternal) {
            std::wstring askMsg =
                reason + L"\n\n"
                L"• [예(Y) - 강력 권장] : 내장 시스템 드라이브(" + sysDrv + L"\\OneTimePE_Cache)로 PE 파일을 자동 복사하여 0xc000000f 오류 없이 안전하게 진행합니다.\n"
                L"• [아니오(N)] : 복사하지 않고 현재 경로 그대로 강제 진행합니다.\n"
                L"• [취소] : 작업을 중단합니다.";
            int ans = MessageBoxW(g_hMainWnd, askMsg.c_str(),
                                  L"외장/특수 드라이브 부팅 오류(0xc000000f) 방지 안내",
                                  MB_ICONWARNING | MB_YESNOCANCEL);
            if (ans == IDCANCEL) return false;
            if (ans == IDYES) {
                if (!StageFilesToInternalCache(outWim, outSdi)) return false;
            } else if (onVirtualDisk || onUnsupportedFs) {
                MessageBoxW(g_hMainWnd,
                            L"가상 드라이브(ISO/VHD) 및 exFAT 볼륨은 부트 매니저가 직접 부팅할 수 없습니다.\n내장 드라이브 자동 복사([예])를 선택하거나 NTFS 내장 드라이브에 복사 후 실행해 주세요.",
                            L"직접 부팅 불가 볼륨", MB_ICONERROR | MB_OK);
                return false;
            }
        } else {
            int ans = MessageBoxW(g_hMainWnd,
                                  (reason + L"\n\n그래도 현재 드라이브에서 강제로 계속 진행하시겠습니까?").c_str(),
                                  L"드라이브 호환성 경고", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
            if (ans != IDYES) return false;
        }
    }

    // 3. Ensure NTFS-compressed WIM/SDI files are uncompressed (NTFS compression breaks bootmgfw RAMDisk with 0xc000000f)
    if (outWim.isCompressedNtfs || outSdi.isCompressedNtfs) {
        AppendLog(L"WIM/SDI 파일에 NTFS 압축 속성이 감지되어 RAMDisk 부팅 오류 방지를 위해 압축을 해제합니다...");
        if (outSdi.isCompressedNtfs) RunSystemTool(L"compact.exe", L"/u \"" + outSdi.fullPath + L"\"");
        if (outWim.isCompressedNtfs) RunSystemTool(L"compact.exe", L"/u \"" + outWim.fullPath + L"\"");
        outWim = ResolvePeFile(g_cfg.wimInput);
        outSdi = ResolvePeFile(g_cfg.sdiInput);
    }

    // 4. Deep WIM binary header & XML manifest validation
    net_sync::WimInspectionInfo wi = net_sync::InspectWimFile(outWim.fullPath);
    if (wi.fileOpened && !wi.validWimMagic) {
        MessageBoxW(g_hMainWnd,
                    (L"선택한 WIM 파일이 올바른 Microsoft WIM 이미지 포맷(MSWIM)이 아니거나 손상되었습니다:\n" +
                     outWim.fullPath + L"\n\n파일 복사 중 손상되었거나 다른 형식의 이미지일 수 있으므로 정상 boot.wim 파일로 교체해 주세요.").c_str(),
                    L"손상되거나 잘못된 WIM 파일", MB_ICONERROR | MB_OK);
        return false;
    }

    if (wi.isSplitSwm) {
        MessageBoxW(g_hMainWnd,
                    L"분할된 WIM(.swm) 이미지는 RAMDisk 단일 부팅을 지원하지 않습니다.\n단일 boot.wim 파일로 병합한 뒤 다시 시도해 주세요.",
                    L"분할 WIM 부팅 미지원", MB_ICONERROR | MB_OK);
        return false;
    }

    if (g_isUefi && wi.arch == 0) {
        MessageBoxW(g_hMainWnd,
                    L"현재 PC는 64비트 UEFI 모드이나 선택한 WIM 파일이 [32비트(x86) WinPE] 이미지입니다.\n\n"
                    L"64비트 UEFI 부트 매니저는 32비트 이미지를 로드할 수 없어 \\windows\\system32\\winload.efi (0xc000000f) 오류가 발생합니다.\n"
                    L"64비트(x64) WinPE boot.wim 파일을 사용해 주세요.",
                    L"32비트(x86) WIM 아키텍처 불일치", MB_ICONERROR | MB_OK);
        return false;
    }

    if (wi.isLzmsEsd) {
        int ans = MessageBoxW(
            g_hMainWnd,
            L"선택한 WIM 파일은 확장자만 .wim이고 내부는 고압축 [ESD (LZMS 솔리드 압축)] 포맷입니다!\n\n"
            L"Windows 부트 매니저(RAMDisk)는 ESD 압축을 직접 읽지 못해 재부팅 시 반드시\n"
            L"\\windows\\system32\\winload.efi (상태: 0xc000000f) 오류가 발생합니다.\n\n"
            L"지금 DISM을 통해 RAMDisk 부팅 표준 WIM(LZX 압축 + Bootable)으로 자동 변환한 뒤 이어서 진행할까요?\n"
            L"(약 30~90초 소요, 변환 중 원본은 안전하게 보호됩니다)",
            L"ESD(LZMS) 압축 포맷 감지 — 자동 변환 안내",
            MB_ICONWARNING | MB_YESNO);
        if (ans == IDYES) {
            uint32_t srcIdx = (wi.bootIndex > 0) ? wi.bootIndex
                                                 : ((wi.imageCount > 0) ? wi.imageCount : 1u);
            StartAsyncEsdConvert(outWim.fullPath, srcIdx, esdFollowUp);
        }
        return false;
    }

    uint32_t activeImgIdx = (wi.bootIndex > 0) ? wi.bootIndex
                                               : ((wi.imageCount > 0) ? wi.imageCount : 1u);

    if (wi.validWimMagic && wi.imageCount > 0 && wi.bootIndex == 0) {
        AppendLog(L"WIM 헤더에 부팅 가능 플래그(BootIndex=0)가 누락되어 있어 자동 설정을 수행합니다...");
        if (net_sync::EnsureWimBootableFlag(outWim.fullPath, activeImgIdx,
                                            [](const std::wstring& m) { AppendLog(m); })) {
            outWim = ResolvePeFile(g_cfg.wimInput);
            UpdateFileStatusBanner();
        }
    }

    if (!wi.systemRoot.empty() && _wcsicmp(wi.systemRoot.c_str(), g_cfg.systemRoot.c_str()) != 0) {
        AppendLog(L"WIM 메타데이터에 맞춰 SystemRoot 자동 보정: " + g_cfg.systemRoot + L" -> " + wi.systemRoot);
        g_cfg.systemRoot = wi.systemRoot;
        SetWindowTextW(g_hEdtSysRoot, g_cfg.systemRoot.c_str());
    }

    std::wstring preferredLoader = net_sync::DetectPreferredLoaderPathInWim(
        outWim.fullPath, activeImgIdx, g_isUefi, g_cfg.loaderPath);
    if (!preferredLoader.empty() && _wcsicmp(preferredLoader.c_str(), g_cfg.loaderPath.c_str()) != 0) {
        AppendLog(L"WIM 내부 실제 부트 로더 위치 감지 — 경로 자동 보정: " + g_cfg.loaderPath + L" -> " + preferredLoader);
        g_cfg.loaderPath = preferredLoader;
        SetWindowTextW(g_hCmbLoader, g_cfg.loaderPath.c_str());
    }

    if (outWim.sizeBytes > (3ULL * 1024ULL * 1024ULL * 1024ULL)) {
        wchar_t warnBuf[512];
        swprintf_s(warnBuf,
                   L"선택한 WIM 파일 크기가 %.2f GB로 매우 큽니다.\n"
                   L"Windows 설치 본체(install.wim)이거나 초고용량 이미지일 경우, UEFI RAMDisk 메모리 한계로 인해 부팅 시 0xc000000f 또는 0xc0000017 오류가 발생할 수 있습니다. (일반 WinPE용 boot.wim 권장)\n\n"
                   L"그래도 계속 진행하시겠습니까?",
                   static_cast<double>(outWim.sizeBytes) / (1024.0 * 1024.0 * 1024.0));
        if (MessageBoxW(g_hMainWnd, warnBuf, L"대용량 WIM 이미지 경고",
                        MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
            return false;
        }
    }

    return true;
}

bool RegisterOrUpdateBcdEntries(ResolvedFileInfo& wimInfo,
                                ResolvedFileInfo& sdiInfo,
                                bool alreadyValidated = false) {
    if (!alreadyValidated) {
        if (!ValidateInputsAndFiles(wimInfo, sdiInfo, PostPatchFollowUp::ContinueApplyBcdOnly)) {
            return false;
        }
    }
    SaveConfigToIni(false);

    AppendLog(L"--- BCD 부팅 구성 등록/갱신 시작 ---");

    if (!DoesBcdGuidExist(g_cfg.sdiGuid)) {
        std::wstring sdiDesc = g_cfg.description + L" Ramdisk";
        CmdResult r = RunSystemTool(L"bcdedit.exe", L"/create /d \"" + sdiDesc + L"\" /device");
        std::wstring newGuid = ExtractFirstGuid(r.output);
        if (r.exitCode != 0 || newGuid.empty()) {
            MessageBoxW(g_hMainWnd, L"RAMDisk 디바이스 항목 생성에 실패했습니다. 하단 로그를 확인하세요.",
                        L"BCD 오류", MB_ICONERROR | MB_OK);
            return false;
        }
        g_cfg.sdiGuid = newGuid;
        SaveConfigToIni(false);
    } else {
        std::wstring sdiDesc = g_cfg.description + L" Ramdisk";
        RunSystemTool(L"bcdedit.exe", L"/set \"" + g_cfg.sdiGuid + L"\" description \"" + sdiDesc + L"\"");
    }

    if (RunSystemTool(L"bcdedit.exe", L"/set \"" + g_cfg.sdiGuid + L"\" ramdisksdidevice \"partition=" + sdiInfo.driveColon + L"\"").exitCode != 0 ||
        RunSystemTool(L"bcdedit.exe", L"/set \"" + g_cfg.sdiGuid + L"\" ramdisksdipath \"" + sdiInfo.bcdRelPath + L"\"").exitCode != 0) {
        MessageBoxW(g_hMainWnd, L"boot.sdi 경로 설정에 실패했습니다. 하단 로그를 확인하세요.",
                    L"BCD 오류", MB_ICONERROR | MB_OK);
        return false;
    }

    if (!DoesBcdGuidExist(g_cfg.peGuid)) {
        CmdResult r = RunSystemTool(L"bcdedit.exe", L"/create /d \"" + g_cfg.description + L"\" /application osloader");
        std::wstring newGuid = ExtractFirstGuid(r.output);
        if (r.exitCode != 0 || newGuid.empty()) {
            MessageBoxW(g_hMainWnd, L"WinPE OS 로더 항목 생성에 실패했습니다. 하단 로그를 확인하세요.",
                        L"BCD 오류", MB_ICONERROR | MB_OK);
            return false;
        }
        g_cfg.peGuid = newGuid;
        SaveConfigToIni(false);
    } else {
        RunSystemTool(L"bcdedit.exe", L"/set \"" + g_cfg.peGuid + L"\" description \"" + g_cfg.description + L"\"");
    }

    std::wstring ramdiskSpec = L"ramdisk=[" + wimInfo.driveColon + L"]" + wimInfo.bcdRelPath + L"," + g_cfg.sdiGuid;

    bool ok = true;
    ok = ok && (RunSystemTool(L"bcdedit.exe", L"/set \"" + g_cfg.peGuid + L"\" device \"" + ramdiskSpec + L"\"").exitCode == 0);
    ok = ok && (RunSystemTool(L"bcdedit.exe", L"/set \"" + g_cfg.peGuid + L"\" osdevice \"" + ramdiskSpec + L"\"").exitCode == 0);
    ok = ok && (RunSystemTool(L"bcdedit.exe", L"/set \"" + g_cfg.peGuid + L"\" path \"" + g_cfg.loaderPath + L"\"").exitCode == 0);
    ok = ok && (RunSystemTool(L"bcdedit.exe", L"/set \"" + g_cfg.peGuid + L"\" systemroot \"" + g_cfg.systemRoot + L"\"").exitCode == 0);
    ok = ok && (RunSystemTool(L"bcdedit.exe", std::wstring(L"/set \"") + g_cfg.peGuid + L"\" winpe " + (g_cfg.winpeMode ? L"yes" : L"no")).exitCode == 0);
    ok = ok && (RunSystemTool(L"bcdedit.exe", std::wstring(L"/set \"") + g_cfg.peGuid + L"\" detecthal " + (g_cfg.detectHal ? L"yes" : L"no")).exitCode == 0);

    if (!ok) {
        MessageBoxW(g_hMainWnd, L"WinPE 부팅 변수 설정 중 오류가 발생했습니다. 하단 로그를 확인하세요.",
                    L"BCD 오류", MB_ICONERROR | MB_OK);
        return false;
    }

    if (!g_cfg.nxPolicy.empty()) {
        RunSystemTool(L"bcdedit.exe", L"/set \"" + g_cfg.peGuid + L"\" nx " + g_cfg.nxPolicy);
    } else {
        RunSystemTool(L"bcdedit.exe", L"/deletevalue \"" + g_cfg.peGuid + L"\" nx", false);
    }

    if (!g_cfg.extraSetArgs.empty()) {
        g_secureBootState = DetectSecureBootState();
        size_t start = 0;
        while (start < g_cfg.extraSetArgs.size()) {
            size_t semi = g_cfg.extraSetArgs.find(L';', start);
            std::wstring token = Trim(semi == std::wstring::npos
                                      ? g_cfg.extraSetArgs.substr(start)
                                      : g_cfg.extraSetArgs.substr(start, semi - start));
            if (!token.empty()) {
                if (RunSystemTool(L"bcdedit.exe", L"/set \"" + g_cfg.peGuid + L"\" " + token).exitCode != 0) {
                    std::wstring lowTok = ToLower(token);
                    bool isSbProtectedOpt = (lowTok.rfind(L"nointegritychecks", 0) == 0 ||
                                             lowTok.rfind(L"testsigning", 0) == 0);
                    if (isSbProtectedOpt && g_isUefi && g_secureBootState == 1) {
                        AppendLog(L"참고: 현재 UEFI Secure Boot가 켜져 있어 보안 정책에 의해 '" + token +
                                  L"' 옵션은 생략되었습니다. (서명되지 않은 개조 커스텀 PE 부팅 시 0xc000000f/0xc0000428 오류가 나면 BIOS[F2]에서 Secure Boot를 꺼주세요.)");
                    } else {
                        MessageBoxW(g_hMainWnd, (L"추가 변수 적용 실패: " + token).c_str(),
                                    L"추가 변수 오류", MB_ICONWARNING | MB_OK);
                        return false;
                    }
                }
            }
            if (semi == std::wstring::npos) break;
            start = semi + 1;
        }
    }

    SaveConfigToIni(false);
    RefreshBcdStatusUI();
    AppendLog(L"BCD 등록 및 변수 적용 완료. (PE GUID: " + g_cfg.peGuid + L")");
    return true;
}

bool IsVolumeBitLockerProtected(const std::wstring& driveColon, int cachedState) {
    if (driveColon.empty()) return false;
    if (cachedState == 1 || cachedState == 3) return true;
    if (cachedState == 0 || cachedState == 2 || cachedState == 5 || cachedState == 8) return false;
    if (g_isAdmin) {
        CmdResult r = RunSystemTool(L"manage-bde.exe", L"-status " + driveColon, false);
        if (r.output.find(L"Protection On") != std::wstring::npos ||
            r.output.find(L"보호 켜짐") != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

bool HandleBitLockerBeforeBoot(const ResolvedFileInfo& wimInfo, const ResolvedFileInfo& sdiInfo) {
    bool shouldSuspend = g_cfg.suspendBitLocker;
    bool wimBlActive   = IsVolumeBitLockerProtected(wimInfo.driveColon, wimInfo.bitLockerState);
    bool sdiBlActive   = (_wcsicmp(sdiInfo.driveColon.c_str(), wimInfo.driveColon.c_str()) == 0)
                         ? wimBlActive
                         : IsVolumeBitLockerProtected(sdiInfo.driveColon, sdiInfo.bitLockerState);
    bool blActiveOnTarget = wimBlActive || sdiBlActive;

    if (!shouldSuspend && blActiveOnTarget) {
        std::wstring targetDrv = wimBlActive ? wimInfo.driveColon : sdiInfo.driveColon;
        std::wstring msg =
            L"대상 드라이브(" + targetDrv + L")에 현재 BitLocker(또는 Windows 장치 암호화) 보호가 켜져 있습니다.\n"
            L"보호를 일시 중지하지 않으면 UEFI 부트 매니저가 PE 파일을 읽지 못해 \\windows\\system32\\winload.efi (0xc000000f) 오류가 발생합니다.\n\n"
            L"BitLocker 보호를 자동으로 일시 중지하고 안전하게 진행할까요?\n"
            L"([예]: 일시 중지 후 진행  /  [아니오]: 일시 중지 없이 진행  /  [취소]: 작업 중단)";
        int ans = MessageBoxW(g_hMainWnd, msg.c_str(), L"BitLocker 암호화 감지", MB_ICONWARNING | MB_YESNOCANCEL);
        if (ans == IDCANCEL) return false;
        if (ans == IDYES) {
            shouldSuspend = true;
            SendMessageW(g_hChkBitLocker, BM_SETCHECK, BST_CHECKED, 0);
            g_cfg.suspendBitLocker = true;
        }
    }

    if (shouldSuspend) {
        std::wstring sysDrv = GetSystemDriveColon();
        std::vector<std::wstring> drives = { wimInfo.driveColon };
        if (_wcsicmp(sdiInfo.driveColon.c_str(), wimInfo.driveColon.c_str()) != 0) {
            drives.push_back(sdiInfo.driveColon);
        }
        for (const auto& drv : drives) {
            int st = QueryVolumeBitLockerState(drv);
            if (IsVolumeBitLockerProtected(drv, st)) {
                bool isOsVolume = (_wcsicmp(drv.c_str(), sysDrv.c_str()) == 0);
                CmdResult r{};
                if (isOsVolume && g_cfg.bitLockerRebootCount > 0) {
                    std::wstring args = L"-protectors -disable " + drv +
                                        L" -RebootCount " + std::to_wstring(g_cfg.bitLockerRebootCount);
                    r = RunSystemTool(L"manage-bde.exe", args);
                    if (r.exitCode != 0) {
                        AppendLog(L"RebootCount 지정 일시 중지 실패 — 표준 일시 중지 명령으로 재시도합니다 (" + drv + L")...");
                        r = RunSystemTool(L"manage-bde.exe", L"-protectors -disable " + drv);
                    }
                } else {
                    if (!isOsVolume) {
                        AppendLog(L"비OS 데이터 볼륨(" + drv + L") BitLocker 보호 일시 중지 실행 (-RebootCount 제외)...");
                    }
                    r = RunSystemTool(L"manage-bde.exe", L"-protectors -disable " + drv);
                }

                if (r.exitCode != 0) {
                    int cont = MessageBoxW(
                        g_hMainWnd,
                        (L"BitLocker 일시 중지 명령이 실패했습니다 (" + drv +
                         L").\n이 상태로 재부팅하면 0xc000000f 부팅 오류가 발생할 수 있습니다.\n그래도 계속 진행하시겠습니까?").c_str(),
                        L"BitLocker 일시 중지 실패", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
                    if (cont != IDYES) return false;
                }
            } else if (st == 5) {
                AppendLog(L"드라이브 " + drv + L"는 이미 BitLocker 보호가 일시 중지된 상태입니다.");
            } else {
                AppendLog(L"드라이브 " + drv + L"는 활성 BitLocker 잠금 상태가 아니므로 일시 중지를 건너뜁니다.");
            }
        }
        UpdateFileStatusBanner();
    }
    return true;
}

void ActionQueueOneTimeBoot(bool rebootImmediately, bool skipUnpatchedPrompt = false) {
    PostPatchFollowUp followUp = rebootImmediately ? PostPatchFollowUp::ContinueBootNow
                                                   : PostPatchFollowUp::ContinueQueueOnly;
    ResolvedFileInfo wimInfo{}, sdiInfo{};
    if (!ValidateInputsAndFiles(wimInfo, sdiInfo, followUp)) return;

    // Check unattended network auto-apply option & WIM patch state
    if (g_cfg.autoApplyNetwork && !skipUnpatchedPrompt && !IsWimCurrentlyPatched(wimInfo)) {
        int ans = MessageBoxW(
            g_hMainWnd,
            L"'PE 부팅 시 현재 네트워크 자동 적용' 옵션이 켜져 있으나,\n"
            L"현재 선택한 WIM 파일에 아직 [무인 자동 실행 패치]가 적용되어 있지 않습니다.\n\n"
            L"• [예(Y)] : 지금 자동으로 WIM 무인 패치(최초 1회)를 먼저 적용한 뒤 이어서 부팅 예약을 진행합니다.\n"
            L"• [아니오(N)] : WIM 수정 없이 네트워크 설정/드라이버만 추출한 뒤 진행합니다.\n"
            L"• [취소] : 작업을 중단합니다.",
            L"WIM 무인 네트워크 패치 안내",
            MB_ICONQUESTION | MB_YESNOCANCEL);
        if (ans == IDCANCEL) return;
        if (ans == IDYES) {
            StartAsyncWimPatch(followUp);
            return;
        }
    }

    net_sync::WimInspectionInfo wi = net_sync::InspectWimFile(wimInfo.fullPath);
    bool warnOldPeSecureBoot = (g_isUefi && g_secureBootState == 1 && wi.osBuild > 0 && wi.osBuild < 22000);
    if (warnOldPeSecureBoot) {
        AppendLog(L"참고: UEFI Secure Boot가 켜져 있으며 선택한 WIM은 구형 빌드(Build " +
                  std::to_wstring(wi.osBuild) +
                  L")입니다. 최신 보안 패치(KB5025885 등)가 적용된 노트북에서 차단될 경우 BIOS에서 Secure Boot를 잠시 끄거나 Win11 기반 PE를 사용하세요.");
    }

    if (rebootImmediately) {
        std::wstring sbNote;
        if (warnOldPeSecureBoot) {
            sbNote = L"\n⚠ 참고: 현재 Secure Boot가 켜져 있고 구형 PE(Build " + std::to_wstring(wi.osBuild) +
                     L")가 선택되어 있습니다. 부팅이 차단될 경우 Win11 PE를 쓰거나 BIOS에서 Secure Boot를 꺼주세요.\n";
        }
        bool wimPatched = IsWimCurrentlyPatched(wimInfo);
        wchar_t confirmMsg[1280];
        swprintf_s(confirmMsg,
                   L"설정을 저장하고 다음 1회 재부팅을 WinPE로 예약한 뒤 %d초 후 재부팅합니다.\n\n"
                   L"• WIM: %s\n"
                   L"• 로더: %s\n"
                   L"• 드라이브(C:) 자동 복구: %s\n"
                   L"• 네트워크 무인 적용: %s\n%s\n"
                   L"PE에서 작업 완료 후 재부팅하면 자동으로 기존 Windows로 복귀합니다.\n"
                   L"지금 재부팅하시겠습니까?",
                   g_cfg.rebootDelaySec,
                   wimInfo.fullPath.c_str(),
                   g_cfg.loaderPath.c_str(),
                   wimPatched ? L"활성화됨 (패치 적용됨)" : L"미적용",
                   g_cfg.autoApplyNetwork ? L"활성화됨 (드라이버 및 IP/DNS/Wi-Fi 동기화)" : L"비활성",
                   sbNote.c_str());
        if (MessageBoxW(g_hMainWnd, confirmMsg, L"1회성 WinPE 부팅 실행 확인",
                        MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
            AppendLog(L"사용자가 재부팅을 취소했습니다.");
            return;
        }
    }

    if (g_cfg.autoApplyNetwork) {
        std::wstring wimParentDir;
        size_t sl = wimInfo.fullPath.find_last_of(L"\\/");
        if (sl != std::wstring::npos) {
            wimParentDir = wimInfo.fullPath.substr(0, sl);
        }
        bool useCacheExport = !wimParentDir.empty() &&
                              (ToLower(wimParentDir).find(L"onetimepe_cache") != std::wstring::npos);
        ActionSyncWindowsNetworkNow(false, useCacheExport ? wimParentDir : L"");
    } else {
        SyncDriveMapAndBridgeForPe(wimInfo.fullPath);
    }

    // Suspend BitLocker BEFORE modifying BCD so protectors/PCR state and Clear Key are cleanly prepared
    if (!HandleBitLockerBeforeBoot(wimInfo, sdiInfo)) return;
    if (!RegisterOrUpdateBcdEntries(wimInfo, sdiInfo, true)) return;

    CmdResult seqRes = RunSystemTool(L"bcdedit.exe", L"/bootsequence \"" + g_cfg.peGuid + L"\"");
    if (seqRes.exitCode != 0) {
        MessageBoxW(g_hMainWnd, L"1회성 부팅 시퀀스(/bootsequence) 예약에 실패했습니다.",
                    L"예약 실패", MB_ICONERROR | MB_OK);
        return;
    }

    RefreshBcdStatusUI();
    AppendLog(L"★ 1회성 WinPE 부팅 예약 완료! (다음 재부팅 1회만 적용되며 이후 자동 해제됨)");

    if (rebootImmediately) {
        std::wstring shutArgs = L"/r /t " + std::to_wstring(g_cfg.rebootDelaySec) +
                                L" /c \"OneTimePE: 1회성 WinPE 부팅을 위해 시스템을 재시작합니다.\"";
        CmdResult sr = RunSystemTool(L"shutdown.exe", shutArgs);
        if (sr.exitCode != 0) {
            MessageBoxW(g_hMainWnd, L"재부팅 명령(shutdown.exe) 실행에 실패했습니다. 직접 시스템을 다시 시작해 주세요.",
                        L"재부팅 알림", MB_ICONWARNING | MB_OK);
        } else if (g_cfg.rebootDelaySec > 0) {
            AppendLog(L"재부팅 카운트다운이 시작되었습니다. 취소하려면 [예약취소/삭제] 버튼을 누르세요.");
        }
    } else {
        MessageBoxW(g_hMainWnd,
                    L"다음 재부팅 시 1회성으로 WinPE로 진입하도록 예약되었습니다!\n원하실 때 Windows '다시 시작'을 누르면 PE로 부팅되며, 다시 재부팅하면 기존 Windows로 돌아옵니다.",
                    L"1회성 부팅 예약 완료", MB_ICONINFORMATION | MB_OK);
    }
}

void ActionCancelOrCleanBcd() {
    RunSystemTool(L"shutdown.exe", L"/a", false);

    int choice = MessageBoxW(
        g_hMainWnd,
        L"수행할 작업을 선택하세요:\n\n"
        L"• [예(Y)] : 1회성 부팅 예약 취소 + 등록된 PE/SDI BCD 항목 및 임시 캐시 정리\n"
        L"• [아니오(N)] : BCD 항목은 남겨두고 '1회성 부팅 예약(bootsequence)'만 취소\n"
        L"• [취소] : 닫기",
        L"예약 취소 및 BCD 정리", MB_ICONQUESTION | MB_YESNOCANCEL);

    if (choice == IDCANCEL) return;

    RunSystemTool(L"bcdedit.exe", L"/deletevalue {bootmgr} bootsequence");

    if (choice == IDYES) {
        if (!g_cfg.peGuid.empty() && DoesBcdGuidExist(g_cfg.peGuid)) {
            RunSystemTool(L"bcdedit.exe", L"/delete \"" + g_cfg.peGuid + L"\" /cleanup");
        }
        if (!g_cfg.sdiGuid.empty() && DoesBcdGuidExist(g_cfg.sdiGuid)) {
            RunSystemTool(L"bcdedit.exe", L"/delete \"" + g_cfg.sdiGuid + L"\"");
        }
        g_cfg.peGuid.clear();
        g_cfg.sdiGuid.clear();

        std::wstring cacheDir = GetSystemDriveColon() + L"\\OneTimePE_Cache";
        if (_wcsicmp(g_appDir.c_str(), cacheDir.c_str()) != 0 &&
            GetFileAttributesW(cacheDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            RemoveDirectoryTree(cacheDir);
            if (ToLower(GetWndText(g_hCmbWim)).find(L"onetimepe_cache") != std::wstring::npos ||
                ToLower(GetWndText(g_hCmbSdi)).find(L"onetimepe_cache") != std::wstring::npos) {
                SetWindowTextW(g_hCmbWim, L"boot.wim");
                SetWindowTextW(g_hCmbSdi, L"boot.sdi");
                ScanLocalPeFiles(true);
            }
            AppendLog(L"내장 임시 캐시 폴더 정리 완료: " + cacheDir);
        }

        SaveConfigToIni(false);
        AppendLog(L"1회성 예약 취소 및 BCD 등록 항목 완전 삭제가 완료되었습니다.");
    } else {
        AppendLog(L"1회성 부팅 예약(bootsequence)이 취소되었습니다.");
    }

    RefreshBcdStatusUI();
}

void RecreateFonts() {
    if (g_hFontNormal) DeleteObject(g_hFontNormal);
    if (g_hFontBold)   DeleteObject(g_hFontBold);
    if (g_hFontTitle)  DeleteObject(g_hFontTitle);
    if (g_hFontMono)   DeleteObject(g_hFontMono);

    g_hFontNormal = CreateFontW(-Scale(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
    g_hFontBold   = CreateFontW(-Scale(13), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
    g_hFontTitle  = CreateFontW(-Scale(17), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
    g_hFontMono   = CreateFontW(-Scale(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
}

void ApplyFontsToControls() {
    const HWND normalCtrls[] = {
        g_hLblSysInfo, g_hLblAppDir, g_hEdtAppDir, g_hBtnOpenDir, g_hBtnRescan,
        g_hLblWim, g_hCmbWim, g_hBtnBrowseWim, g_hLblSdi, g_hCmbSdi, g_hBtnBrowseSdi, g_hBtnGenSdi,
        g_hLblFileStatus, g_hLblDesc, g_hEdtDesc, g_hLblSysRoot, g_hEdtSysRoot,
        g_hLblLoader, g_hCmbLoader, g_hChkWinpe, g_hChkDetectHal, g_hLblNx, g_hCmbNx,
        g_hLblSecTitle, g_hBtnCheckBl,
        g_hChkBitLocker, g_hLblBlCount, g_hEdtBlCount, g_hLblRebootSec, g_hEdtRebootSec,
        g_hLblExtra, g_hEdtExtra,
        g_hChkAutoNet, g_hChkExportDrv, g_hBtnSyncNet, g_hBtnPatchWim,
        g_hLblBcdStatus, g_hBtnRefreshBcd,
        g_hBtnSaveCfg, g_hBtnApplyBcd, g_hBtnQueueOnly, g_hBtnCleanBcd, g_hBtnClearLog
    };
    for (HWND h : normalCtrls) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontNormal), TRUE);
    }

    const HWND boldCtrls[] = {
        g_hGrpFiles, g_hGrpVars, g_hGrpNet, g_hGrpActions, g_hGrpLog,
        g_hBtnImportCustom, g_hLblSecStatus, g_hLblNetStatus, g_hBtnBootNow
    };
    for (HWND h : boldCtrls) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontBold), TRUE);
    }

    if (g_hLblTitle) SendMessageW(g_hLblTitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontTitle), TRUE);
    if (g_hEdtLog)   SendMessageW(g_hEdtLog, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFontMono), TRUE);
}

void LayoutControls(int clientW, int clientH) {
    const int margin = Scale(12);
    const int contentW = std::max(Scale(660), clientW - margin * 2);
    const int pad = Scale(12);
    const int rowH = Scale(24);
    const int labelW = Scale(80);

    HDWP hdwp = BeginDeferWindowPos(60);
    auto place = [&](HWND h, int x, int y, int w, int hgt) {
        if (h) hdwp = DeferWindowPos(hdwp, h, nullptr, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE);
    };

    // Header
    int sysInfoW = Scale(325);
    place(g_hLblTitle, margin, Scale(9), contentW - sysInfoW - Scale(6), Scale(26));
    place(g_hLblSysInfo, margin + contentW - sysInfoW, Scale(13), sysInfoW, Scale(20));

    // Group 1: PE Files
    int g1Y = Scale(40);
    int g1H = Scale(144);
    place(g_hGrpFiles, margin, g1Y, contentW, g1H);

    int inX = margin + pad;
    int inW = contentW - pad * 2;
    int btnW = Scale(90);
    int btnGap = Scale(6);

    int r1Y = g1Y + Scale(22);
    int importBtnW = Scale(146);
    int dirBtnW    = Scale(80);
    place(g_hLblAppDir, inX, r1Y + Scale(3), labelW, Scale(20));
    int edtDirW = inW - labelW - (importBtnW + dirBtnW * 2 + btnGap * 3);
    place(g_hEdtAppDir, inX + labelW, r1Y, edtDirW, rowH);
    int r1BtnX = inX + labelW + edtDirW + btnGap;
    place(g_hBtnImportCustom, r1BtnX, r1Y, importBtnW, rowH);
    place(g_hBtnOpenDir, r1BtnX + importBtnW + btnGap, r1Y, dirBtnW, rowH);
    place(g_hBtnRescan, r1BtnX + importBtnW + btnGap * 2 + dirBtnW, r1Y, dirBtnW, rowH);

    int r2Y = r1Y + Scale(30);
    place(g_hLblWim, inX, r2Y + Scale(3), labelW, Scale(20));
    int cmbWimW = inW - labelW - (btnW + btnGap);
    place(g_hCmbWim, inX + labelW, r2Y, cmbWimW, Scale(200));
    place(g_hBtnBrowseWim, inX + labelW + cmbWimW + btnGap, r2Y, btnW, rowH);

    int r3Y = r2Y + Scale(30);
    int genBtnW = Scale(108);
    place(g_hLblSdi, inX, r3Y + Scale(3), labelW, Scale(20));
    int cmbSdiW = inW - labelW - (btnW + genBtnW + btnGap * 2);
    place(g_hCmbSdi, inX + labelW, r3Y, cmbSdiW, Scale(200));
    place(g_hBtnBrowseSdi, inX + labelW + cmbSdiW + btnGap, r3Y, btnW, rowH);
    place(g_hBtnGenSdi, inX + labelW + cmbSdiW + btnGap * 2 + btnW, r3Y, genBtnW, rowH);

    int r4Y = r3Y + Scale(31);
    place(g_hLblFileStatus, inX, r4Y, inW, Scale(20));

    // Group 2: Boot Variables & Security Diagnostics
    int g2Y = g1Y + g1H + Scale(7);
    int g2H = Scale(210);
    place(g_hGrpVars, margin, g2Y, contentW, g2H);

    int vr1Y = g2Y + Scale(23);
    int sysRootBlkW = Scale(210);
    place(g_hLblDesc, inX, vr1Y + Scale(3), labelW, Scale(20));
    int descW = inW - labelW - sysRootBlkW - Scale(12);
    place(g_hEdtDesc, inX + labelW, vr1Y, descW, rowH);
    int srX = inX + labelW + descW + Scale(12);
    place(g_hLblSysRoot, srX, vr1Y + Scale(3), Scale(84), Scale(20));
    place(g_hEdtSysRoot, srX + Scale(84), vr1Y, sysRootBlkW - Scale(84), rowH);

    int vr2Y = vr1Y + Scale(30);
    place(g_hLblLoader, inX, vr2Y + Scale(3), labelW, Scale(20));
    place(g_hCmbLoader, inX + labelW, vr2Y, inW - labelW, Scale(200));

    int vr3Y = vr2Y + Scale(30);
    place(g_hChkWinpe, inX + labelW, vr3Y + Scale(2), Scale(185), Scale(22));
    place(g_hChkDetectHal, inX + labelW + Scale(195), vr3Y + Scale(2), Scale(195), Scale(22));
    int nxX = inX + labelW + Scale(400);
    place(g_hLblNx, nxX, vr3Y + Scale(3), Scale(60), Scale(20));
    place(g_hCmbNx, nxX + Scale(60), vr3Y, std::max(Scale(110), inX + inW - (nxX + Scale(60))), Scale(200));

    int vr4Y = vr3Y + Scale(30);
    int chkBlBtnW = Scale(138);
    place(g_hLblSecTitle, inX, vr4Y + Scale(3), labelW, Scale(20));
    place(g_hLblSecStatus, inX + labelW, vr4Y + Scale(3), inW - labelW - chkBlBtnW - btnGap, Scale(20));
    place(g_hBtnCheckBl, inX + inW - chkBlBtnW, vr4Y - Scale(1), chkBlBtnW, rowH);

    int vr5Y = vr4Y + Scale(29);
    place(g_hChkBitLocker, inX + labelW, vr5Y + Scale(2), Scale(245), Scale(22));
    int blCntX = inX + labelW + Scale(252);
    place(g_hLblBlCount, blCntX, vr5Y + Scale(3), Scale(72), Scale(20));
    place(g_hEdtBlCount, blCntX + Scale(72), vr5Y, Scale(42), rowH);
    int rbSecX = blCntX + Scale(128);
    place(g_hLblRebootSec, rbSecX, vr5Y + Scale(3), Scale(95), Scale(20));
    place(g_hEdtRebootSec, rbSecX + Scale(95), vr5Y, Scale(48), rowH);

    int vr6Y = vr5Y + Scale(30);
    place(g_hLblExtra, inX, vr6Y + Scale(3), labelW, Scale(20));
    place(g_hEdtExtra, inX + labelW, vr6Y, inW - labelW, rowH);

    // Group 3: PE Auto-Network & Unattended WIM Patch
    int g3Y = g2Y + g2H + Scale(7);
    int g3H = Scale(84);
    place(g_hGrpNet, margin, g3Y, contentW, g3H);

    int nr1Y = g3Y + Scale(22);
    place(g_hLblNetStatus, inX, nr1Y + Scale(2), inW, Scale(20));

    int nr2Y = nr1Y + Scale(27);
    int netBtnW1 = Scale(132);
    int netBtnW2 = Scale(142);
    place(g_hChkAutoNet, inX, nr2Y + Scale(2), Scale(238), Scale(22));
    place(g_hChkExportDrv, inX + Scale(244), nr2Y + Scale(2), Scale(188), Scale(22));
    place(g_hBtnSyncNet, inX + inW - netBtnW1 - netBtnW2 - btnGap, nr2Y - Scale(1), netBtnW1, Scale(25));
    place(g_hBtnPatchWim, inX + inW - netBtnW2, nr2Y - Scale(1), netBtnW2, Scale(25));

    // Group 4: BCD Status & Actions
    int g4Y = g3Y + g3H + Scale(7);
    int g4H = Scale(88);
    place(g_hGrpActions, margin, g4Y, contentW, g4H);

    int ar1Y = g4Y + Scale(21);
    place(g_hLblBcdStatus, inX, ar1Y + Scale(2), inW - Scale(100), Scale(20));
    place(g_hBtnRefreshBcd, inX + inW - Scale(94), ar1Y - Scale(1), Scale(94), Scale(23));

    int ar2Y = ar1Y + Scale(27);
    int actGap = Scale(6);
    int smallBtnW = Scale(108);
    int bootBtnW = std::max(Scale(175), inW - (smallBtnW * 4 + actGap * 4));
    int curX = inX;
    place(g_hBtnSaveCfg, curX, ar2Y, smallBtnW, Scale(31)); curX += smallBtnW + actGap;
    place(g_hBtnApplyBcd, curX, ar2Y, smallBtnW, Scale(31)); curX += smallBtnW + actGap;
    place(g_hBtnQueueOnly, curX, ar2Y, smallBtnW, Scale(31)); curX += smallBtnW + actGap;
    place(g_hBtnCleanBcd, curX, ar2Y, smallBtnW, Scale(31)); curX += smallBtnW + actGap;
    place(g_hBtnBootNow, curX, ar2Y, bootBtnW, Scale(31));

    // Group 5: Execution Log (stretches to bottom)
    int g5Y = g4Y + g4H + Scale(7);
    int g5H = std::max(Scale(105), clientH - g5Y - margin);
    place(g_hGrpLog, margin, g5Y, contentW, g5H);
    place(g_hBtnClearLog, margin + contentW - Scale(92) - pad, g5Y + Scale(14), Scale(92), Scale(22));
    place(g_hEdtLog, inX, g5Y + Scale(39), inW, std::max(Scale(52), g5H - Scale(48)));

    EndDeferWindowPos(hdwp);
    InvalidateRect(g_hMainWnd, nullptr, TRUE);
}

void PopulateInitialValues() {
    SetWindowTextW(g_hEdtAppDir, g_appDir.c_str());

    std::wstring sbStr = L"해당없음";
    if (g_isUefi) {
        sbStr = (g_secureBootState == 1) ? L"켜짐" : ((g_secureBootState == 0) ? L"꺼짐" : L"미확인");
    }
    std::wstring sysInfo = std::wstring(L"펌웨어: ") + (g_isUefi ? L"UEFI" : L"Legacy BIOS") +
                           L"  |  Secure Boot: " + sbStr +
                           L"  |  권한: " + (g_isAdmin ? L"관리자" : L"제한됨");
    SetWindowTextW(g_hLblSysInfo, sysInfo.c_str());

    SendMessageW(g_hCmbLoader, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"\\windows\\system32\\winload.efi"));
    SendMessageW(g_hCmbLoader, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"\\windows\\system32\\boot\\winload.efi"));
    SendMessageW(g_hCmbLoader, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"\\windows\\system32\\winload.exe"));
    SendMessageW(g_hCmbLoader, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"\\windows\\system32\\boot\\winload.exe"));
    SetWindowTextW(g_hCmbLoader, g_cfg.loaderPath.c_str());

    SendMessageW(g_hCmbNx, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"기본값(미지정)"));
    SendMessageW(g_hCmbNx, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"OptIn"));
    SendMessageW(g_hCmbNx, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"OptOut"));
    SendMessageW(g_hCmbNx, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"AlwaysOff"));
    SendMessageW(g_hCmbNx, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"AlwaysOn"));

    int nxSel = 0;
    if (_wcsicmp(g_cfg.nxPolicy.c_str(), L"OptIn") == 0) nxSel = 1;
    else if (_wcsicmp(g_cfg.nxPolicy.c_str(), L"OptOut") == 0) nxSel = 2;
    else if (_wcsicmp(g_cfg.nxPolicy.c_str(), L"AlwaysOff") == 0) nxSel = 3;
    else if (_wcsicmp(g_cfg.nxPolicy.c_str(), L"AlwaysOn") == 0) nxSel = 4;
    SendMessageW(g_hCmbNx, CB_SETCURSEL, nxSel, 0);

    SetWindowTextW(g_hEdtDesc, g_cfg.description.c_str());
    SetWindowTextW(g_hEdtSysRoot, g_cfg.systemRoot.c_str());
    SendMessageW(g_hChkWinpe, BM_SETCHECK, g_cfg.winpeMode ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_hChkDetectHal, BM_SETCHECK, g_cfg.detectHal ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_hChkBitLocker, BM_SETCHECK, g_cfg.suspendBitLocker ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(g_hEdtBlCount, std::to_wstring(g_cfg.bitLockerRebootCount).c_str());
    SetWindowTextW(g_hEdtRebootSec, std::to_wstring(g_cfg.rebootDelaySec).c_str());
    SetWindowTextW(g_hEdtExtra, g_cfg.extraSetArgs.c_str());
    SendMessageW(g_hEdtExtra, EM_SETCUEBANNER, FALSE,
                 reinterpret_cast<LPARAM>(L"선택 입력 — 예: nointegritychecks yes; bootmenupolicy Legacy"));

    SendMessageW(g_hChkAutoNet, BM_SETCHECK, g_cfg.autoApplyNetwork ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_hChkExportDrv, BM_SETCHECK, g_cfg.exportNicDrivers ? BST_CHECKED : BST_UNCHECKED, 0);

    ScanLocalPeFiles(false);
    RefreshBcdStatusUI();

    wchar_t exeBuf[MAX_PATH * 4]{};
    DWORD exeLen = GetModuleFileNameW(nullptr, exeBuf, _countof(exeBuf));
    std::wstring selfPath(exeBuf, exeLen);

    AppendLog(std::wstring(L"OneTimePE ") + kAppVersion + L" [빌드 시각: " + kBuildTimestamp + L"] 실행 완료.");
    AppendLog(L"실행 파일 경로: " + selfPath);
    AppendLog(L"시스템 보안 상태 — " + sysInfo);
    if (g_resetFromOtherPc) {
        AppendLog(L"알림: 다른 PC에서 복사된 설정(OneTimePE.ini)이 감지되어 이전 BCD GUID를 초기화하고 현재 PC 펌웨어에 맞게 로더 경로를 보정했습니다.");
    }
    AppendLog(L"커스텀 PE 사용 시 상단 [★ 커스텀PE 가져오기] 버튼을 누르거나 .iso / 폴더를 드래그 앤 드롭하세요.");
}

void CreateAllControls(HWND hWnd) {
    auto mkStatic = [&](const wchar_t* txt, DWORD extraStyle, int id = 0) {
        return CreateWindowExW(0, L"STATIC", txt, WS_CHILD | WS_VISIBLE | extraStyle,
                               0, 0, 10, 10, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    };
    auto mkGroup = [&](const wchar_t* txt) {
        return CreateWindowExW(0, L"BUTTON", txt, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                               0, 0, 10, 10, hWnd, nullptr, g_hInst, nullptr);
    };
    auto mkBtn = [&](const wchar_t* txt, int id, DWORD extra = 0) {
        return CreateWindowExW(0, L"BUTTON", txt, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | extra,
                               0, 0, 10, 10, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    };
    auto mkEdit = [&](int id, DWORD extra = ES_AUTOHSCROLL) {
        return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra,
                               0, 0, 10, 10, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    };
    auto mkCombo = [&](int id, DWORD comboStyle) {
        return CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | comboStyle,
                               0, 0, 10, 10, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    };
    auto mkCheck = [&](const wchar_t* txt, int id) {
        return CreateWindowExW(0, L"BUTTON", txt, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                               0, 0, 10, 10, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hInst, nullptr);
    };

    g_hLblTitle   = mkStatic(L"OneTimePE v1.3 - 1회성 내장 WinPE & 커스텀PE 매니저", SS_LEFT, IDC_LBL_TITLE);
    g_hLblSysInfo = mkStatic(L"", SS_RIGHT, IDC_LBL_SYSINFO);

    // Group 1
    g_hGrpFiles        = mkGroup(L"1. PE 파일 인식 (같은 폴더 자동 탐색 / 커스텀PE .iso · 폴더 · 파일 드래그 앤 드롭 지원)");
    g_hLblAppDir       = mkStatic(L"현재 폴더:", SS_LEFT);
    g_hEdtAppDir       = mkEdit(IDC_EDT_APPDIR, ES_AUTOHSCROLL | ES_READONLY);
    g_hBtnImportCustom = mkBtn(L"★ 커스텀PE 가져오기", IDC_BTN_IMPORT_CUSTOM);
    g_hBtnOpenDir      = mkBtn(L"폴더 열기", IDC_BTN_OPENDIR);
    g_hBtnRescan       = mkBtn(L"다시 스캔", IDC_BTN_RESCAN);

    g_hLblWim        = mkStatic(L"WIM 파일:", SS_LEFT);
    g_hCmbWim        = mkCombo(IDC_CMB_WIM, CBS_DROPDOWN | CBS_AUTOHSCROLL);
    g_hBtnBrowseWim  = mkBtn(L"찾아보기...", IDC_BTN_BROWSE_WIM);

    g_hLblSdi        = mkStatic(L"SDI 파일:", SS_LEFT);
    g_hCmbSdi        = mkCombo(IDC_CMB_SDI, CBS_DROPDOWN | CBS_AUTOHSCROLL);
    g_hBtnBrowseSdi  = mkBtn(L"찾아보기...", IDC_BTN_BROWSE_SDI);
    g_hBtnGenSdi     = mkBtn(L"내장 SDI 생성", IDC_BTN_GEN_SDI);

    g_hLblFileStatus = mkStatic(L"", SS_LEFT | SS_PATHELLIPSIS, IDC_LBL_FILESTATUS);

    // Group 2
    g_hGrpVars       = mkGroup(L"2. 부팅 변수 및 암호화/보안(BitLocker · Secure Boot) 설정");
    g_hLblDesc       = mkStatic(L"부팅 이름:", SS_LEFT);
    g_hEdtDesc       = mkEdit(IDC_EDT_DESC);
    g_hLblSysRoot    = mkStatic(L"SystemRoot:", SS_LEFT);
    g_hEdtSysRoot    = mkEdit(IDC_EDT_SYSROOT);

    g_hLblLoader     = mkStatic(L"로더 경로:", SS_LEFT);
    g_hCmbLoader     = mkCombo(IDC_CMB_LOADER, CBS_DROPDOWN | CBS_AUTOHSCROLL);

    g_hChkWinpe      = mkCheck(L"WinPE 모드 (winpe = yes)", IDC_CHK_WINPE);
    g_hChkDetectHal  = mkCheck(L"HAL 감지 (detecthal = yes)", IDC_CHK_DETECTHAL);
    g_hLblNx         = mkStatic(L"NX 정책:", SS_LEFT);
    g_hCmbNx         = mkCombo(IDC_CMB_NX, CBS_DROPDOWNLIST);

    g_hLblSecTitle   = mkStatic(L"보안/암호화:", SS_LEFT);
    g_hLblSecStatus  = mkStatic(L"", SS_LEFT | SS_ENDELLIPSIS, IDC_LBL_SECSTATUS);
    g_hBtnCheckBl    = mkBtn(L"BitLocker 상세 확인", IDC_BTN_CHECK_BL);

    g_hChkBitLocker  = mkCheck(L"재부팅 전 BitLocker 보호 일시 중지", IDC_CHK_BITLOCKER);
    g_hLblBlCount    = mkStatic(L"해제 횟수:", SS_LEFT);
    g_hEdtBlCount    = mkEdit(IDC_EDT_BL_COUNT, ES_NUMBER | ES_CENTER);
    g_hLblRebootSec  = mkStatic(L"재부팅 대기(초):", SS_LEFT);
    g_hEdtRebootSec  = mkEdit(IDC_EDT_REBOOT_SEC, ES_NUMBER | ES_CENTER);

    g_hLblExtra      = mkStatic(L"추가 변수:", SS_LEFT);
    g_hEdtExtra      = mkEdit(IDC_EDT_EXTRA_SET);

    // Group 3: Drive Letter (C:) Auto-Fix & Unattended Network WIM Patch
    g_hGrpNet        = mkGroup(L"3. PE 부팅 후 드라이브 문자(C:) 자동 복구 & 무인 네트워크 연결 (WIM 패치)");
    g_hLblNetStatus  = mkStatic(L"", SS_LEFT | SS_ENDELLIPSIS, IDC_LBL_NETSTATUS);
    g_hChkAutoNet    = mkCheck(L"PE 부팅 시 현재 네트워크 자동 적용", IDC_CHK_AUTONET);
    g_hChkExportDrv  = mkCheck(L"랜카드 드라이버(.inf) 함께 추출", IDC_CHK_EXPORT_DRV);
    g_hBtnSyncNet    = mkBtn(L"네트워크 확인/추출", IDC_BTN_SYNC_NET);
    g_hBtnPatchWim   = mkBtn(L"WIM 자동복구 패치", IDC_BTN_PATCH_WIM);

    // Group 4: BCD Status & Actions
    g_hGrpActions    = mkGroup(L"4. BCD 상태 및 1회성 부팅 실행");
    g_hLblBcdStatus  = mkStatic(L"", SS_LEFT, IDC_LBL_BCDSTATUS);
    g_hBtnRefreshBcd = mkBtn(L"상태 확인", IDC_BTN_REFRESH_BCD);

    g_hBtnSaveCfg    = mkBtn(L"설정 저장", IDC_BTN_SAVE_CFG);
    g_hBtnApplyBcd   = mkBtn(L"BCD 등록/갱신", IDC_BTN_APPLY_BCD);
    g_hBtnQueueOnly  = mkBtn(L"1회성 예약만", IDC_BTN_QUEUE_ONLY);
    g_hBtnCleanBcd   = mkBtn(L"예약취소/삭제", IDC_BTN_CLEAN_BCD);
    g_hBtnBootNow    = mkBtn(L"▶ 저장 후 지금 PE 재부팅", IDC_BTN_BOOT_NOW, BS_DEFPUSHBUTTON);

    // Group 5: Log
    g_hGrpLog        = mkGroup(L"5. 실행 로그 (bcdedit / DISM / manage-bde / 네트워크 추출 내역)");
    g_hBtnClearLog   = mkBtn(L"로그 지우기", IDC_BTN_CLEAR_LOG);
    g_hEdtLog        = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                       ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                       0, 0, 10, 10, hWnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDT_LOG)), g_hInst, nullptr);
    SendMessageW(g_hEdtLog, EM_SETLIMITTEXT, 0, 0);

    RecreateFonts();
    ApplyFontsToControls();
}

void HandleDroppedFiles(HDROP hDrop) {
    if (g_isTaskBusy) {
        DragFinish(hDrop);
        return;
    }
    std::wstring customImportTarget;
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH * 2]{};
        if (DragQueryFileW(hDrop, i, path, _countof(path)) > 0) {
            std::wstring dropped(path);
            DWORD attr = GetFileAttributesW(dropped.c_str());
            std::wstring low = ToLower(dropped);
            std::wstring fnLow = ToLower(FileNameFromPath(dropped));

            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                customImportTarget = dropped;
                break;
            }
            if ((low.size() >= 4 && low.substr(low.size() - 4) == L".iso") ||
                fnLow == L"bcd" ||
                (low.size() >= 4 && low.substr(low.size() - 4) == L".bcd")) {
                customImportTarget = dropped;
                break;
            }
            if (low.size() >= 4 && low.substr(low.size() - 4) == L".wim") {
                SetWindowTextW(g_hCmbWim, MakeAppRelativeIfInside(dropped).c_str());
                AppendLog(L"드래그 앤 드롭으로 WIM 지정됨: " + dropped);
            } else if (low.size() >= 4 && low.substr(low.size() - 4) == L".sdi") {
                SetWindowTextW(g_hCmbSdi, MakeAppRelativeIfInside(dropped).c_str());
                AppendLog(L"드래그 앤 드롭으로 SDI 지정됨: " + dropped);
            }
        }
    }
    DragFinish(hDrop);
    if (!customImportTarget.empty()) {
        AppendLog(L"드래그 앤 드롭으로 커스텀 PE 미디어 감지됨: " + customImportTarget);
        ImportCustomPeFromPath(customImportTarget, -1);
    } else {
        UpdateFileStatusBanner();
    }
}

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hMainWnd = hWnd;
            UINT wndDpi = GetDpiForWindow(hWnd);
            if (wndDpi > 0) g_dpi = wndDpi;

            DragAcceptFiles(hWnd, TRUE);
            ChangeWindowMessageFilterEx(hWnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
            ChangeWindowMessageFilterEx(hWnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
            ChangeWindowMessageFilterEx(hWnd, 0x0049 /*WM_COPYGLOBALDATA*/, MSGFLT_ALLOW, nullptr);

            CreateAllControls(hWnd);
            PopulateInitialValues();
            return 0;
        }

        case WM_CLOSE: {
            if (g_isTaskBusy) {
                MessageBoxW(hWnd,
                            L"현재 PE 이미지 작업(복사/변환/패치)이 진행 중입니다.\n이미지 손상 방지를 위해 완료될 때까지 잠시만 기다려 주세요.",
                            L"작업 진행 중", MB_ICONWARNING | MB_OK);
                return 0;
            }
            DestroyWindow(hWnd);
            return 0;
        }

        case WM_APP_TASK_LOG: {
            auto* heapStr = reinterpret_cast<std::wstring*>(lParam);
            if (heapStr) {
                AppendLog(*heapStr);
                delete heapStr;
            }
            return 0;
        }

        case WM_APP_TASK_DONE: {
            auto* payload = reinterpret_cast<AsyncWimTaskResult*>(lParam);
            SetTaskBusyState(false);
            if (payload) {
                AsyncWimTaskKind  kind          = payload->kind;
                bool              ok            = payload->success;
                PostPatchFollowUp next          = payload->followUp;
                std::wstring      customSummary = payload->customSummary;
                bool              warnSb        = payload->warnSecureBoot;

                if (ok && (kind == AsyncWimTaskKind::UnattendedNetPatch ||
                           kind == AsyncWimTaskKind::CustomImportDriveFixPatch)) {
                    g_cfg.patchedWimPath = payload->wimPath;
                    g_cfg.patchedWimSig  = payload->newSig;
                    SaveConfigToIni(false);
                }
                UpdateFileStatusBanner();
                delete payload;

                if (ok) {
                    if (kind == AsyncWimTaskKind::ConvertEsdToWim) {
                        if (next == PostPatchFollowUp::ContinueBootNow) {
                            ActionQueueOneTimeBoot(true, false);
                        } else if (next == PostPatchFollowUp::ContinueQueueOnly) {
                            ActionQueueOneTimeBoot(false, false);
                        } else if (next == PostPatchFollowUp::ContinueApplyBcdOnly) {
                            ResolvedFileInfo w{}, s{};
                            if (RegisterOrUpdateBcdEntries(w, s)) {
                                SyncDriveMapAndBridgeForPe(w.fullPath);
                                MessageBoxW(hWnd,
                                            (L"ESD → 표준 WIM 변환 및 시스템 BCD 등록이 완료되었습니다.\n\nPE GUID: " + g_cfg.peGuid).c_str(),
                                            L"변환 및 BCD 등록 완료", MB_ICONINFORMATION | MB_OK);
                            }
                        } else {
                            MessageBoxW(hWnd,
                                        L"ESD(LZMS) 이미지를 RAMDisk 부팅 표준 WIM 포맷으로 변환 완료했습니다!",
                                        L"WIM 포맷 변환 완료", MB_ICONINFORMATION | MB_OK);
                        }
                    } else if (kind == AsyncWimTaskKind::CustomImportDriveFixPatch) {
                        MessageBoxW(hWnd,
                                    customSummary.c_str(),
                                    warnSb ? L"커스텀 PE 가져오기 및 C: 자동복구 패치 완료 (Secure Boot 안내)"
                                           : L"커스텀 PE 가져오기 및 C: 자동복구 패치 완료",
                                    (warnSb ? MB_ICONWARNING : MB_ICONINFORMATION) | MB_OK);
                    } else {
                        if (next == PostPatchFollowUp::ContinueBootNow) {
                            ActionQueueOneTimeBoot(true, true);
                        } else if (next == PostPatchFollowUp::ContinueQueueOnly) {
                            ActionQueueOneTimeBoot(false, true);
                        } else {
                            MessageBoxW(hWnd,
                                        L"선택한 WIM 파일에 [기존 드라이브 문자(C: 등) 자동 복구 + 무인 부팅 패치] 및 로더 보정이 완료되었습니다!\n\n"
                                        L"• PE 부팅 시 숨김/오프라인 속성을 해제하고 원래 Windows 파티션을 C: 로 자동 고정합니다.\n"
                                        L"• 'PE 부팅 시 현재 네트워크 자동 적용' 체크 시 랜카드 드라이버·IP·Wi-Fi도 함께 동기화됩니다.",
                                        L"WIM 자동 복구 & 무인 패치 완료", MB_ICONINFORMATION | MB_OK);
                        }
                    }
                } else {
                    if (kind == AsyncWimTaskKind::ConvertEsdToWim) {
                        MessageBoxW(hWnd,
                                    L"ESD → 표준 WIM 변환 중 오류가 발생했습니다.\n하단 실행 로그에서 상세 원인을 확인해 주세요.",
                                    L"WIM 작업 실패", MB_ICONERROR | MB_OK);
                    } else if (kind == AsyncWimTaskKind::CustomImportDriveFixPatch) {
                        MessageBoxW(hWnd,
                                    L"커스텀 PE 파일 세팅은 완료되었으나, WIM 내부 [C: 드라이브 자동 복구 패치] 적용 중 오류가 발생했습니다.\n"
                                    L"하단 실행 로그에서 상세 원인을 확인해 주세요.\n\n"
                                    L"(패치 없이 부팅 자체는 지금 가능하며, 필요 시 [WIM 자동복구 패치] 버튼으로 재시도할 수 있습니다.)",
                                    L"커스텀 PE 패치 경고", MB_ICONWARNING | MB_OK);
                    } else {
                        MessageBoxW(hWnd,
                                    L"WIM 자동 복구 패치 작업 중 오류가 발생했습니다.\n하단 실행 로그에서 상세 원인을 확인해 주세요.",
                                    L"WIM 작업 실패", MB_ICONERROR | MB_OK);
                    }
                }
            }
            return 0;
        }

        case WM_SIZE: {
            if (wParam != SIZE_MINIMIZED) {
                LayoutControls(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = Scale(720);
            mmi->ptMinTrackSize.y = Scale(730);
            return 0;
        }

        case WM_DPICHANGED: {
            g_dpi = HIWORD(wParam);
            RecreateFonts();
            ApplyFontsToControls();
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hWnd, nullptr,
                         suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            RECT rc{};
            GetClientRect(hWnd, &rc);
            LayoutControls(rc.right - rc.left, rc.bottom - rc.top);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            HWND hCtrl = reinterpret_cast<HWND>(lParam);
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, RGB(246, 248, 250));
            if (hCtrl == g_hLblTitle) {
                SetTextColor(hdc, RGB(15, 76, 146));
            } else if (hCtrl == g_hLblBcdStatus) {
                std::wstring st = GetWndText(g_hLblBcdStatus);
                if (st.find(L"★") != std::wstring::npos) {
                    SetTextColor(hdc, RGB(0, 118, 60));
                } else {
                    SetTextColor(hdc, RGB(35, 45, 55));
                }
            } else if (hCtrl == g_hLblSecStatus) {
                std::wstring st = GetWndText(g_hLblSecStatus);
                if (st.find(L"⚠") != std::wstring::npos) {
                    SetTextColor(hdc, RGB(190, 40, 25));
                } else {
                    SetTextColor(hdc, RGB(15, 110, 60));
                }
            } else if (hCtrl == g_hLblNetStatus) {
                std::wstring st = GetWndText(g_hLblNetStatus);
                if (st.find(L"미적용") != std::wstring::npos || st.find(L"⚠") != std::wstring::npos) {
                    SetTextColor(hdc, RGB(180, 85, 10));
                } else {
                    SetTextColor(hdc, RGB(15, 110, 60));
                }
            } else if (hCtrl == g_hLblFileStatus) {
                std::wstring st = GetWndText(g_hLblFileStatus);
                if (st.find(L"파일 없음") != std::wstring::npos ||
                    st.find(L"이동식") != std::wstring::npos ||
                    st.find(L"⚠") != std::wstring::npos) {
                    SetTextColor(hdc, RGB(185, 40, 30));
                } else {
                    SetTextColor(hdc, RGB(20, 105, 55));
                }
            } else {
                SetTextColor(hdc, RGB(30, 35, 40));
            }
            return reinterpret_cast<LRESULT>(g_hBgBrush);
        }

        case WM_DROPFILES: {
            HandleDroppedFiles(reinterpret_cast<HDROP>(wParam));
            return 0;
        }

        case WM_COMMAND: {
            const int id   = LOWORD(wParam);
            const int code = HIWORD(wParam);

            if ((id == IDC_CMB_WIM || id == IDC_CMB_SDI) &&
                (code == CBN_EDITCHANGE || code == CBN_SELCHANGE)) {
                if (code == CBN_SELCHANGE) {
                    HWND hCmb = reinterpret_cast<HWND>(lParam);
                    int sel = static_cast<int>(SendMessageW(hCmb, CB_GETCURSEL, 0, 0));
                    if (sel != CB_ERR) {
                        int len = static_cast<int>(SendMessageW(hCmb, CB_GETLBTEXTLEN, sel, 0));
                        if (len > 0) {
                            std::wstring item(static_cast<size_t>(len) + 1, L'\0');
                            SendMessageW(hCmb, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(item.data()));
                            item.resize(static_cast<size_t>(len));
                            SetWindowTextW(hCmb, item.c_str());
                        }
                    }
                }
                UpdateFileStatusBanner();
                return 0;
            }

            if (code == BN_CLICKED) {
                switch (id) {
                    case IDC_BTN_IMPORT_CUSTOM:
                        ActionImportCustomPe();
                        break;

                    case IDC_BTN_OPENDIR:
                        ShellExecuteW(hWnd, L"open", g_appDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        break;

                    case IDC_BTN_RESCAN:
                        ScanLocalPeFiles(true);
                        AppendLog(L"현재 폴더 PE 파일 다시 스캔 완료.");
                        break;

                    case IDC_BTN_BROWSE_WIM:
                        BrowseForFile(g_hCmbWim,
                                      L"WIM 이미지 파일 (*.wim)\0*.wim\0모든 파일 (*.*)\0*.*\0",
                                      L"PE WIM 이미지 파일 선택");
                        break;

                    case IDC_BTN_BROWSE_SDI:
                        BrowseForFile(g_hCmbSdi,
                                      L"SDI 디스크 이미지 (*.sdi)\0*.sdi\0모든 파일 (*.*)\0*.*\0",
                                      L"boot.sdi 파일 선택");
                        break;

                    case IDC_BTN_GEN_SDI:
                        CopyBuiltInBootSdi();
                        break;

                    case IDC_BTN_CHECK_BL:
                        ActionCheckBitLockerAndSecureBoot();
                        break;

                    case IDC_BTN_SYNC_NET:
                        ActionSyncWindowsNetworkNow(true);
                        break;

                    case IDC_BTN_PATCH_WIM:
                        StartAsyncWimPatch(PostPatchFollowUp::None);
                        break;

                    case IDC_BTN_REFRESH_BCD:
                        UpdateFileStatusBanner();
                        RefreshBcdStatusUI();
                        AppendLog(L"BCD, 드라이브 보안 및 네트워크 상태를 새로고침했습니다.");
                        break;

                    case IDC_BTN_SAVE_CFG:
                        if (SaveConfigToIni(true)) {
                            MessageBoxW(hWnd,
                                        (L"모든 변수 설정이 저장되었습니다.\n\n저장 위치:\n" + g_iniPath).c_str(),
                                        L"설정 저장 완료", MB_ICONINFORMATION | MB_OK);
                        }
                        break;

                    case IDC_BTN_APPLY_BCD: {
                        ResolvedFileInfo w{}, s{};
                        if (RegisterOrUpdateBcdEntries(w, s)) {
                            SyncDriveMapAndBridgeForPe(w.fullPath);
                            MessageBoxW(hWnd,
                                        (L"시스템 BCD에 WinPE 항목이 등록/갱신되었습니다.\n(멀티부팅 메뉴에는 표시되지 않으며 1회성 부팅 시 사용됩니다.)\n\nPE GUID: " + g_cfg.peGuid).c_str(),
                                        L"BCD 등록 완료", MB_ICONINFORMATION | MB_OK);
                        }
                        break;
                    }

                    case IDC_BTN_QUEUE_ONLY:
                        ActionQueueOneTimeBoot(false);
                        break;

                    case IDC_BTN_CLEAN_BCD:
                        ActionCancelOrCleanBcd();
                        break;

                    case IDC_BTN_BOOT_NOW:
                        ActionQueueOneTimeBoot(true);
                        break;

                    case IDC_BTN_CLEAR_LOG:
                        SetWindowTextW(g_hEdtLog, L"");
                        break;
                }
                return 0;
            }
            break;
        }

        case WM_DESTROY: {
            if (g_hFontNormal) DeleteObject(g_hFontNormal);
            if (g_hFontBold)   DeleteObject(g_hFontBold);
            if (g_hFontTitle)  DeleteObject(g_hFontTitle);
            if (g_hFontMono)   DeleteObject(g_hFontMono);
            if (g_hBgBrush)    DeleteObject(g_hBgBrush);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void InitAppDirOnly() {
    wchar_t exeBuf[MAX_PATH * 4]{};
    DWORD len = GetModuleFileNameW(nullptr, exeBuf, _countof(exeBuf));
    std::wstring exePath(exeBuf, len);
    size_t slash = exePath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        g_appDir = exePath.substr(0, slash);
        if (g_appDir.size() == 2 && g_appDir[1] == L':') {
            g_appDir += L"\\";
        }
    } else {
        g_appDir = L".";
    }
}

void InitPathsAndEnv() {
    InitAppDirOnly();
    g_iniPath         = JoinPath(g_appDir, L"OneTimePE.ini");
    g_machineId       = GetLocalMachineId();
    g_isUefi          = DetectFirmwareIsUefi();
    g_secureBootState = DetectSecureBootState();
    g_isAdmin         = DetectIsAdmin();
    LoadConfigFromIni();
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int nCmdShow) {
    InitAppDirOnly();

    // Silent unattended boot-init / C: drive letter fix / network setup modes inside WinPE
    if (lpCmdLine) {
        if (wcsstr(lpCmdLine, L"--pe-boot-init") != nullptr) {
            return net_sync::RunPeBootInitMode(g_appDir);
        }
        if (wcsstr(lpCmdLine, L"--pe-fix-drives") != nullptr) {
            return net_sync::RunPeFixDrivesMode();
        }
        if (wcsstr(lpCmdLine, L"--pe-apply-net") != nullptr) {
            net_sync::RunPeFixDrivesMode();
            return net_sync::RunPeAutoNetMode(g_appDir);
        }
    }

    g_hInst = hInstance;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    InitPathsAndEnv();
    g_hBgBrush = CreateSolidBrush(RGB(246, 248, 250));

    HICON hIconBig   = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON),
                                                     IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                                     GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    HICON hIconSmall = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON),
                                                     IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                     GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = hIconBig;
    wc.hIconSm       = hIconSmall;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_hBgBrush;
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&wc)) {
        CoUninitialize();
        return 1;
    }

    HDC hScreen = GetDC(nullptr);
    g_dpi = static_cast<UINT>(GetDeviceCaps(hScreen, LOGPIXELSX));
    ReleaseDC(nullptr, hScreen);

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int maxWorkH = (workArea.bottom > workArea.top) ? (workArea.bottom - workArea.top - 16) : 900;

    int initW = Scale(780);
    int initH = std::min(Scale(795), maxWorkH);
    int posX  = std::max<int>(workArea.left, workArea.left + ((workArea.right - workArea.left) - initW) / 2);
    int posY  = std::max<int>(workArea.top, workArea.top + ((workArea.bottom - workArea.top) - initH) / 2);

    HWND hWnd = CreateWindowExW(0, kWindowClassName, kWindowTitle,
                                WS_OVERLAPPEDWINDOW,
                                posX, posY, initW, initH,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) {
        CoUninitialize();
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
