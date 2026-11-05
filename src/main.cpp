#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
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
#include <thread>

#include "resource.h"
#include "net_sync.h"

namespace {

constexpr wchar_t kWindowClassName[] = L"OneTimePE_MainWindow";
constexpr wchar_t kWindowTitle[]     = L"OneTimePE - 1회성 내장 WinPE 부팅 & 무인 네트워크 매니저";
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
    UINT driveType             = DRIVE_UNKNOWN;
    int bitLockerState         = -1;
};

enum class PostPatchFollowUp {
    None,
    ContinueQueueOnly,
    ContinueBootNow
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
bool         g_isUefi          = true;
int          g_secureBootState = -1;
bool         g_isAdmin         = false;
bool         g_isTaskBusy      = false;
AppConfig    g_cfg;

// Controls
HWND g_hLblTitle      = nullptr;
HWND g_hLblSysInfo    = nullptr;

HWND g_hGrpFiles      = nullptr;
HWND g_hLblAppDir     = nullptr;
HWND g_hEdtAppDir     = nullptr;
HWND g_hBtnOpenDir    = nullptr;
HWND g_hBtnRescan     = nullptr;
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

void SetTaskBusyState(bool busy) {
    g_isTaskBusy = busy;
    const HWND interactive[] = {
        g_hCmbWim, g_hBtnBrowseWim, g_hCmbSdi, g_hBtnBrowseSdi, g_hBtnGenSdi, g_hBtnRescan,
        g_hBtnCheckBl, g_hBtnSyncNet, g_hBtnPatchWim,
        g_hBtnRefreshBcd, g_hBtnSaveCfg, g_hBtnApplyBcd, g_hBtnQueueOnly, g_hBtnCleanBcd, g_hBtnBootNow
    };
    for (HWND h : interactive) {
        if (h) EnableWindow(h, busy ? FALSE : TRUE);
    }
    SetWindowTextW(g_hBtnPatchWim, busy ? L"WIM 패치 진행중..." : L"WIM 무인패치 적용");
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
        GetProcAddress(hPropSys, "PSGetPropertyKeyFromName"));
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
    info.isEncryptedEfs = (attrs & FILE_ATTRIBUTE_ENCRYPTED) != 0;

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
        info.bitLockerState = QueryVolumeBitLockerState(info.driveColon);
    }
    return info;
}

bool IsWimCurrentlyPatched(const ResolvedFileInfo& wimInfo) {
    if (!wimInfo.exists || g_cfg.patchedWimPath.empty() || g_cfg.patchedWimSig.empty()) {
        return false;
    }
    if (_wcsicmp(wimInfo.fullPath.c_str(), g_cfg.patchedWimPath.c_str()) != 0) {
        return false;
    }
    std::wstring curSig = net_sync::ComputeFileSignature(wimInfo.fullPath);
    return !curSig.empty() && (curSig == g_cfg.patchedWimSig);
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

    g_cfg.wimInput             = ReadIniStr(L"WimFile", L"boot.wim");
    g_cfg.sdiInput             = ReadIniStr(L"SdiFile", L"boot.sdi");
    g_cfg.description          = ReadIniStr(L"Description", L"One-Time WinPE");
    g_cfg.loaderPath           = ReadIniStr(L"LoaderPath", defaultLoader);
    if (g_cfg.loaderPath.empty()) g_cfg.loaderPath = defaultLoader;
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
    g_cfg.patchedWimPath       = ReadIniStr(L"PatchedWimPath", L"");
    g_cfg.patchedWimSig        = ReadIniStr(L"PatchedWimSig", L"");
    g_cfg.sdiGuid              = ReadIniStr(L"SdiGuid", L"");
    g_cfg.peGuid               = ReadIniStr(L"PeGuid", L"");
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

    std::wstring txt = (net.foundAny ? L"✔ 연결됨: " : L"⚠ ") + net.shortBannerText + L"  |  WIM 무인패치: ";
    if (!wimInfo.exists) {
        txt += L"WIM 미선택";
    } else if (IsWimCurrentlyPatched(wimInfo)) {
        txt += L"✔ 패치 완료";
    } else {
        txt += L"[미적용 — 최초 1회 패치 필요]";
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
        status += L"[WIM 인식됨: " + std::wstring(szBuf) + L"]";
    } else {
        status += L"[WIM 파일 없음 - 같은 폴더에 .wim 파일을 넣어주세요]";
    }

    status += L"  |  ";

    if (sdiInfo.exists) {
        wchar_t szBuf[64];
        swprintf_s(szBuf, L"%.1f MB", static_cast<double>(sdiInfo.sizeBytes) / (1024.0 * 1024.0));
        status += L"[SDI 인식됨: " + std::wstring(szBuf) + L"]";
    } else {
        status += L"[SDI 파일 없음 - 우측 '내장 SDI 생성' 클릭 가능]";
    }

    std::wstring targetDrive = wimInfo.exists ? wimInfo.driveColon
                             : (sdiInfo.exists ? sdiInfo.driveColon : GetAppDirDriveColon());
    UINT targetDrvType = DRIVE_UNKNOWN;
    int blState = -1;
    if (!targetDrive.empty()) {
        std::wstring rootSlash = targetDrive + L"\\";
        targetDrvType = GetDriveTypeW(rootSlash.c_str());
        blState = QueryVolumeBitLockerState(targetDrive);

        status += L"  |  볼륨 [" + targetDrive + L"] ";
        if (targetDrvType == DRIVE_FIXED) {
            status += L"(내장 고정 디스크)";
        } else if (targetDrvType == DRIVE_REMOVABLE) {
            status += L"(주의: 이동식 드라이브)";
        } else {
            status += L"(주의: 특수 드라이브)";
        }
    }

    SetWindowTextW(g_hLblFileStatus, status.c_str());

    if (g_hLblSecStatus) {
        g_secureBootState = DetectSecureBootState();
        std::wstring secTxt;
        if (!targetDrive.empty()) {
            if (blState == 1 || blState == 6) {
                secTxt += L"⚠ [" + targetDrive + L"] BitLocker: " + DescribeBitLockerState(blState);
            } else {
                secTxt += L"✔ [" + targetDrive + L"] BitLocker: " + DescribeBitLockerState(blState);
            }
        } else {
            secTxt += L"대상 볼륨 미지정";
        }

        secTxt += L"  |  Secure Boot: ";
        if (g_secureBootState == 1) {
            secTxt += L"켜짐 (정식 서명 PE 가능)";
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
        report += L" • 호환성 안내: 정품 Windows 부트 매니저를 사용하므로 Microsoft 서명이 유지된 표준/최신 WinPE는 Secure Boot가 켜진 상태에서도 정상 부팅됩니다. (서명이 변조된 일부 구형 개조 PE만 차단됨)\n\n";
    } else {
        report += L" • 펌웨어 모드: UEFI  |  Secure Boot: 꺼짐 (Disabled)\n";
        report += L" • 호환성 안내: 서명 제한 없이 모든 UEFI WinPE 부팅이 가능합니다.\n\n";
    }

    report += L"[2] 전체 드라이브 BitLocker 설정 현황\n";
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
            int st = QueryVolumeBitLockerState(dColon);
            std::wstring line = L" • " + dColon + L" ";
            if (volLabel[0] != L'\0') line += L"(" + std::wstring(volLabel) + L", ";
            else line += L"(";
            line += (fsName[0] != L'\0' ? std::wstring(fsName) : L"알수없음");
            line += (dtype == DRIVE_FIXED ? L", 내장) : " : L", 이동식) : ");
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
        if (targetBlState == 1) {
            report += L" ⚠ 판정: BitLocker 암호화 및 보호가 [켜져 있음]\n";
            report += L" → 조치: PE 부팅 시 파일을 읽으려면 '재부팅 전 BitLocker 보호 일시 중지' 옵션을 체크하세요.\n";
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

    if (targetBlState == 1 && SendMessageW(g_hChkBitLocker, BM_GETCHECK, 0, 0) != BST_CHECKED) {
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

void ActionSyncWindowsNetworkNow(bool showMessageBox) {
    PullControlsToConfig();
    std::wstring relAppDir = GetNormalizedBcdRelativeDir(g_appDir);
    std::wstring summary;
    bool ok = net_sync::ExportNetworkConfigForPe(
        g_appDir, relAppDir, g_cfg.exportNicDrivers,
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
            L"선택한 WIM 이미지에 '무인 네트워크 자동 실행 패치'를 적용하시겠습니까?\n\n"
            L"• 대상 파일: " + wimInfo.fullPath + L"\n"
            L"• 수행 작업: DISM 마운트 → startnet.cmd / winpeshl.ini / Startup 자동 실행 훅 주입 → 저장(Commit)\n"
            L"• 최초 1회만 적용하면 이후에는 재패치 없이 매 부팅마다 최신 네트워크가 자동 연결됩니다.\n\n"
            L"계속하시겠습니까? (약 20~60초 소요)";
        if (MessageBoxW(g_hMainWnd, ask.c_str(), L"WIM 무인 자동 실행 패치 확인",
                        MB_ICONQUESTION | MB_YESNO) != IDYES) {
            return;
        }
    }

    SetTaskBusyState(true);
    std::wstring appDirCopy  = g_appDir;
    std::wstring wimPathCopy = wimInfo.fullPath;

    struct PatchDonePayload {
        bool success = false;
        std::wstring wimPath;
        std::wstring newSig;
        PostPatchFollowUp followUp = PostPatchFollowUp::None;
    };

    std::thread([appDirCopy, wimPathCopy, followUp]() {
        auto* payload = new PatchDonePayload();
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
    auto pOpenDlg = reinterpret_cast<FnGetOpenFileNameW>(GetProcAddress(hComDlg, "GetOpenFileNameW"));
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

bool ValidateInputsAndFiles(ResolvedFileInfo& outWim, ResolvedFileInfo& outSdi) {
    PullControlsToConfig();
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

    if (outWim.driveColon.empty() || outSdi.driveColon.empty()) {
        MessageBoxW(g_hMainWnd,
                    L"네트워크 경로(UNC)에서는 부트 매니저가 파일을 읽을 수 없습니다. 로컬 내장 드라이브에서 실행해 주세요.",
                    L"지원되지 않는 경로", MB_ICONERROR | MB_OK);
        return false;
    }

    if (outWim.isEncryptedEfs || outSdi.isEncryptedEfs) {
        MessageBoxW(g_hMainWnd,
                    L"WIM 또는 SDI 파일에 NTFS 파일 암호화(EFS)가 걸려 있어 부트 매니저가 읽을 수 없습니다.\n파일 속성 -> 고급에서 '내용을 암호화하여 데이터 보호'를 해제해 주세요.",
                    L"EFS 암호화 감지", MB_ICONERROR | MB_OK);
        return false;
    }

    if (outWim.driveType != DRIVE_FIXED || outSdi.driveType != DRIVE_FIXED) {
        int ans = MessageBoxW(g_hMainWnd,
                              L"현재 파일이 내장 고정 디스크(Fixed Disk)가 아닌 드라이브에 위치해 있습니다.\n그래도 계속 진행하시겠습니까?",
                              L"드라이브 유형 확인", MB_ICONWARNING | MB_YESNO);
        if (ans != IDYES) return false;
    }

    return true;
}

bool RegisterOrUpdateBcdEntries(ResolvedFileInfo& wimInfo, ResolvedFileInfo& sdiInfo) {
    if (!ValidateInputsAndFiles(wimInfo, sdiInfo)) return false;
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
        size_t start = 0;
        while (start < g_cfg.extraSetArgs.size()) {
            size_t semi = g_cfg.extraSetArgs.find(L';', start);
            std::wstring token = Trim(semi == std::wstring::npos
                                      ? g_cfg.extraSetArgs.substr(start)
                                      : g_cfg.extraSetArgs.substr(start, semi - start));
            if (!token.empty()) {
                if (RunSystemTool(L"bcdedit.exe", L"/set \"" + g_cfg.peGuid + L"\" " + token).exitCode != 0) {
                    MessageBoxW(g_hMainWnd, (L"추가 변수 적용 실패: " + token).c_str(),
                                L"추가 변수 오류", MB_ICONWARNING | MB_OK);
                    return false;
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

bool HandleBitLockerBeforeBoot(const ResolvedFileInfo& wimInfo, const ResolvedFileInfo& sdiInfo) {
    bool shouldSuspend = g_cfg.suspendBitLocker;
    bool blActiveOnTarget = (wimInfo.bitLockerState == 1) || (sdiInfo.bitLockerState == 1);

    if (!shouldSuspend && blActiveOnTarget) {
        std::wstring targetDrv = (wimInfo.bitLockerState == 1) ? wimInfo.driveColon : sdiInfo.driveColon;
        std::wstring msg =
            L"대상 드라이브(" + targetDrv + L")에 현재 BitLocker 보호가 켜져 있습니다.\n"
            L"보호를 일시 중지하지 않으면 부팅 단계에서 PE 파일을 읽지 못할 수 있습니다.\n\n"
            L"재부팅 " + std::to_wstring(g_cfg.bitLockerRebootCount) +
            L"회 동안 BitLocker 보호를 자동으로 일시 중지하고 진행할까요?\n"
            L"([예]: 일시 중지 후 진행  /  [아니오]: 일시 중지 없이 진행  /  [취소]: 작업 중단)";
        int ans = MessageBoxW(g_hMainWnd, msg.c_str(), L"BitLocker 암호화 감지", MB_ICONWARNING | MB_YESNOCANCEL);
        if (ans == IDCANCEL) return false;
        if (ans == IDYES) shouldSuspend = true;
    }

    if (shouldSuspend) {
        std::vector<std::wstring> drives = { wimInfo.driveColon };
        if (_wcsicmp(sdiInfo.driveColon.c_str(), wimInfo.driveColon.c_str()) != 0) {
            drives.push_back(sdiInfo.driveColon);
        }
        for (const auto& drv : drives) {
            int st = QueryVolumeBitLockerState(drv);
            if (st == 1) {
                std::wstring args = L"-protectors -disable " + drv +
                                    L" -RebootCount " + std::to_wstring(g_cfg.bitLockerRebootCount);
                CmdResult r = RunSystemTool(L"manage-bde.exe", args);
                if (r.exitCode != 0) {
                    int cont = MessageBoxW(g_hMainWnd,
                                           (L"BitLocker 일시 중지 명령이 실패했습니다 (" + drv + L").\n계속 진행하시겠습니까?").c_str(),
                                           L"BitLocker 경고", MB_ICONWARNING | MB_YESNO);
                    if (cont != IDYES) return false;
                }
            } else {
                AppendLog(L"드라이브 " + drv + L"는 활성 BitLocker 잠금 상태가 아니므로 일시 중지를 건너뜁니다.");
            }
        }
        UpdateFileStatusBanner();
    }
    return true;
}

void ActionQueueOneTimeBoot(bool rebootImmediately, bool skipUnpatchedPrompt = false) {
    ResolvedFileInfo wimInfo{}, sdiInfo{};
    if (!ValidateInputsAndFiles(wimInfo, sdiInfo)) return;

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
            StartAsyncWimPatch(rebootImmediately ? PostPatchFollowUp::ContinueBootNow
                                                 : PostPatchFollowUp::ContinueQueueOnly);
            return;
        }
    }

    if (g_cfg.autoApplyNetwork) {
        ActionSyncWindowsNetworkNow(false);
    }

    if (!RegisterOrUpdateBcdEntries(wimInfo, sdiInfo)) return;
    if (!HandleBitLockerBeforeBoot(wimInfo, sdiInfo)) return;

    if (rebootImmediately) {
        wchar_t confirmMsg[640];
        swprintf_s(confirmMsg,
                   L"설정을 저장하고 다음 1회 재부팅을 WinPE로 예약한 뒤 %d초 후 재부팅합니다.\n\n"
                   L"• WIM: %s\n"
                   L"• 로더: %s\n"
                   L"• 네트워크 무인 적용: %s\n\n"
                   L"PE에서 작업 완료 후 재부팅하면 자동으로 기존 Windows로 복귀합니다.\n"
                   L"지금 재부팅하시겠습니까?",
                   g_cfg.rebootDelaySec,
                   wimInfo.fullPath.c_str(),
                   g_cfg.loaderPath.c_str(),
                   g_cfg.autoApplyNetwork ? L"활성화됨 (드라이버 및 IP/DNS/Wi-Fi 동기화)" : L"비활성");
        if (MessageBoxW(g_hMainWnd, confirmMsg, L"1회성 WinPE 부팅 실행 확인",
                        MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
            AppendLog(L"사용자가 재부팅을 취소했습니다.");
            return;
        }
    }

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
        L"• [예(Y)] : 1회성 부팅 예약 취소 + 등록된 PE/SDI BCD 항목까지 모두 깨끗하게 삭제\n"
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
        g_hLblSecStatus, g_hLblNetStatus, g_hBtnBootNow
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

    HDWP hdwp = BeginDeferWindowPos(55);
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
    place(g_hLblAppDir, inX, r1Y + Scale(3), labelW, Scale(20));
    int edtDirW = inW - labelW - (btnW * 2 + btnGap * 2);
    place(g_hEdtAppDir, inX + labelW, r1Y, edtDirW, rowH);
    place(g_hBtnOpenDir, inX + labelW + edtDirW + btnGap, r1Y, btnW, rowH);
    place(g_hBtnRescan, inX + labelW + edtDirW + btnGap * 2 + btnW, r1Y, btnW, rowH);

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
                 reinterpret_cast<LPARAM>(L"선택 입력 — 예: ems no; bootmenupolicy Legacy"));

    SendMessageW(g_hChkAutoNet, BM_SETCHECK, g_cfg.autoApplyNetwork ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_hChkExportDrv, BM_SETCHECK, g_cfg.exportNicDrivers ? BST_CHECKED : BST_UNCHECKED, 0);

    ScanLocalPeFiles(false);
    RefreshBcdStatusUI();

    AppendLog(L"OneTimePE 실행 완료. 앱 폴더: " + g_appDir);
    AppendLog(L"시스템 보안 상태 — " + sysInfo);
    AppendLog(L"최초 1회 [WIM 무인패치 적용]을 수행하면, PE 부팅 직후 자동으로 현재 네트워크와 랜카드 드라이버가 적용됩니다.");
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

    g_hLblTitle   = mkStatic(L"OneTimePE - 1회성 내장 WinPE & 무인 네트워크 매니저", SS_LEFT, IDC_LBL_TITLE);
    g_hLblSysInfo = mkStatic(L"", SS_RIGHT, IDC_LBL_SYSINFO);

    // Group 1
    g_hGrpFiles      = mkGroup(L"1. PE 파일 인식 (실행 파일과 같은 폴더 자동 탐색 / 파일 드래그 앤 드롭 지원)");
    g_hLblAppDir     = mkStatic(L"현재 폴더:", SS_LEFT);
    g_hEdtAppDir     = mkEdit(IDC_EDT_APPDIR, ES_AUTOHSCROLL | ES_READONLY);
    g_hBtnOpenDir    = mkBtn(L"폴더 열기", IDC_BTN_OPENDIR);
    g_hBtnRescan     = mkBtn(L"다시 스캔", IDC_BTN_RESCAN);

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

    // Group 3: Network Auto-Apply & Unattended WIM Patch
    g_hGrpNet        = mkGroup(L"3. PE 부팅 후 무인 네트워크 자동 연결 (현재 Windows 네트워크 읽기 + boot.wim 패치)");
    g_hLblNetStatus  = mkStatic(L"", SS_LEFT | SS_ENDELLIPSIS, IDC_LBL_NETSTATUS);
    g_hChkAutoNet    = mkCheck(L"PE 부팅 시 현재 네트워크 자동 적용", IDC_CHK_AUTONET);
    g_hChkExportDrv  = mkCheck(L"랜카드 드라이버(.inf) 함께 추출", IDC_CHK_EXPORT_DRV);
    g_hBtnSyncNet    = mkBtn(L"네트워크 확인/추출", IDC_BTN_SYNC_NET);
    g_hBtnPatchWim   = mkBtn(L"WIM 무인패치 적용", IDC_BTN_PATCH_WIM);

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
    UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH]{};
        if (DragQueryFileW(hDrop, i, path, _countof(path)) > 0) {
            std::wstring dropped(path);
            std::wstring low = ToLower(dropped);
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
    UpdateFileStatusBanner();
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
                            L"현재 WIM 이미지 마운트/패치 작업이 진행 중입니다.\n이미지 손상 방지를 위해 완료될 때까지 잠시만 기다려 주세요.",
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
            struct PatchDonePayload {
                bool success;
                std::wstring wimPath;
                std::wstring newSig;
                PostPatchFollowUp followUp;
            };
            auto* payload = reinterpret_cast<PatchDonePayload*>(lParam);
            SetTaskBusyState(false);
            if (payload) {
                bool ok = payload->success;
                PostPatchFollowUp next = payload->followUp;
                if (ok) {
                    g_cfg.patchedWimPath = payload->wimPath;
                    g_cfg.patchedWimSig  = payload->newSig;
                    SaveConfigToIni(false);
                }
                UpdateFileStatusBanner();
                delete payload;

                if (ok) {
                    if (next == PostPatchFollowUp::ContinueBootNow) {
                        ActionQueueOneTimeBoot(true, true);
                    } else if (next == PostPatchFollowUp::ContinueQueueOnly) {
                        ActionQueueOneTimeBoot(false, true);
                    } else {
                        MessageBoxW(hWnd,
                                    L"선택한 WIM 파일에 무인 네트워크 자동 실행 패치가 성공적으로 완료되었습니다!\n\n"
                                    L"이제 PE로 부팅하면 자동으로 랜카드 드라이버 적재 및 IP/DNS/Wi-Fi 설정이 적용됩니다.",
                                    L"WIM 무인 패치 완료", MB_ICONINFORMATION | MB_OK);
                    }
                } else {
                    MessageBoxW(hWnd,
                                L"WIM 무인 패치 작업 중 오류가 발생했습니다.\n하단 실행 로그에서 상세 원인을 확인해 주세요.",
                                L"WIM 패치 실패", MB_ICONERROR | MB_OK);
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
                if (st.find(L"파일 없음") != std::wstring::npos || st.find(L"이동식") != std::wstring::npos) {
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
    g_isUefi          = DetectFirmwareIsUefi();
    g_secureBootState = DetectSecureBootState();
    g_isAdmin         = DetectIsAdmin();
    LoadConfigFromIni();
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int nCmdShow) {
    InitAppDirOnly();

    // Silent unattended network setup mode inside WinPE
    if (lpCmdLine && wcsstr(lpCmdLine, L"--pe-apply-net") != nullptr) {
        return net_sync::RunPeAutoNetMode(g_appDir);
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
