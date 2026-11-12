#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winioctl.h>
#include <virtdisk.h>
#include <iphlpapi.h>
#include <wlanapi.h>
#include <tlhelp32.h>

#include "net_sync.h"

#include <algorithm>
#include <cstdlib>
#include <stdlib.h>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <map>
#include <set>

namespace net_sync {
namespace {

std::wstring Trim(const std::wstring& s) {
    size_t start = 0;
    while (start < s.size() && std::iswspace(s[start])) ++start;
    size_t end = s.size();
    while (end > start && std::iswspace(s[end - 1])) --end;
    return s.substr(start, end - start);
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring ToUpper(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
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

std::wstring FileNameFromPath(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) return path.substr(slash + 1);
    return path;
}

void EnsureDirectoryRecursive(const std::wstring& dirPath) {
    if (dirPath.empty()) return;
    DWORD attr = GetFileAttributesW(dirPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return;

    size_t pos = 0;
    if (dirPath.size() >= 2 && dirPath[1] == L':') pos = 3;
    while (pos < dirPath.size()) {
        size_t slash = dirPath.find_first_of(L"\\/", pos);
        std::wstring sub = (slash == std::wstring::npos) ? dirPath : dirPath.substr(0, slash);
        if (!sub.empty()) {
            CreateDirectoryW(sub.c_str(), nullptr);
        }
        if (slash == std::wstring::npos) break;
        pos = slash + 1;
    }
}

void DeleteDirectoryContentsRecursive(const std::wstring& dirPath) {
    DWORD attr = GetFileAttributesW(dirPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;

    std::wstring pattern = JoinPath(dirPath, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring child = JoinPath(dirPath, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
            SetFileAttributesW(child.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DeleteDirectoryContentsRecursive(child);
            RemoveDirectoryW(child.c_str());
        } else {
            DeleteFileW(child.c_str());
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

std::wstring FormatIpv4Sockaddr(const SOCKADDR* sa) {
    if (!sa || sa->sa_family != AF_INET) return L"";
    const auto* sin = reinterpret_cast<const sockaddr_in*>(sa);
    const auto& b   = sin->sin_addr.S_un.S_un_b;
    wchar_t buf[32];
    swprintf_s(buf, L"%u.%u.%u.%u", b.s_b1, b.s_b2, b.s_b3, b.s_b4);
    return buf;
}

std::wstring PrefixLengthToMask(UINT8 prefix) {
    uint32_t p = std::min<uint32_t>(prefix, 32);
    uint32_t mask = (p == 0) ? 0u : (~0u << (32u - p));
    wchar_t buf[32];
    swprintf_s(buf, L"%u.%u.%u.%u",
               (mask >> 24) & 0xFF, (mask >> 16) & 0xFF, (mask >> 8) & 0xFF, mask & 0xFF);
    return buf;
}

std::wstring FormatMacDash(const BYTE* addr, ULONG len) {
    if (!addr || len == 0) return L"";
    std::wstring out;
    for (ULONG i = 0; i < len; ++i) {
        wchar_t b[8];
        swprintf_s(b, (i == 0) ? L"%02X" : L"-%02X", addr[i]);
        out += b;
    }
    return out;
}

bool IsUsableIpv4(const std::wstring& ip) {
    if (ip.empty() || ip == L"0.0.0.0") return false;
    if (ip.rfind(L"127.", 0) == 0) return false;
    if (ip.rfind(L"169.254.", 0) == 0) return false; // APIPA link-local
    return true;
}

std::wstring AnsiToWide(const char* s) {
    if (!s || !*s) return L"";
    int len = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (len <= 1) return L"";
    std::wstring w(static_cast<size_t>(len) - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), len);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::string WideToOemOrAcp(const std::wstring& w, UINT cp = CP_ACP) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(cp, 0, w.data(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(cp, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring DecodeConsoleBytes(const std::string& bytes) {
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

struct PhysicalNicRegInfo {
    std::wstring guidUpper;
    std::wstring driverDesc;
    std::wstring infPath;
    bool         isWifi = false;
};

std::vector<PhysicalNicRegInfo> EnumeratePhysicalNicsFromRegistry() {
    std::vector<PhysicalNicRegInfo> result;
    HKEY hClass = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}",
                      0, KEY_READ, &hClass) != ERROR_SUCCESS) {
        return result;
    }

    wchar_t subKeyName[128];
    for (DWORD idx = 0; ; ++idx) {
        DWORD nameLen = _countof(subKeyName);
        if (RegEnumKeyExW(hClass, idx, subKeyName, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        HKEY hSub = nullptr;
        if (RegOpenKeyExW(hClass, subKeyName, 0, KEY_READ, &hSub) != ERROR_SUCCESS) continue;

        DWORD characteristics = 0;
        DWORD cb = sizeof(characteristics);
        DWORD type = 0;
        bool isPhysical = false;
        if (RegQueryValueExW(hSub, L"Characteristics", nullptr, &type,
                             reinterpret_cast<LPBYTE>(&characteristics), &cb) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            isPhysical = (characteristics & 0x04) != 0; // NCF_PHYSICAL
        }

        if (isPhysical) {
            wchar_t guidBuf[128]{};
            wchar_t descBuf[256]{};
            wchar_t infBuf[128]{};
            DWORD ifType = 0;

            cb = sizeof(guidBuf);
            RegQueryValueExW(hSub, L"NetCfgInstanceId", nullptr, nullptr, reinterpret_cast<LPBYTE>(guidBuf), &cb);
            cb = sizeof(descBuf);
            RegQueryValueExW(hSub, L"DriverDesc", nullptr, nullptr, reinterpret_cast<LPBYTE>(descBuf), &cb);
            cb = sizeof(infBuf);
            RegQueryValueExW(hSub, L"InfPath", nullptr, nullptr, reinterpret_cast<LPBYTE>(infBuf), &cb);
            cb = sizeof(ifType);
            RegQueryValueExW(hSub, L"*IfType", nullptr, nullptr, reinterpret_cast<LPBYTE>(&ifType), &cb);

            std::wstring infLow = ToLower(infBuf);
            // Exclude Bluetooth PAN
            if (guidBuf[0] != L'\0' && infLow != L"bthpan.inf") {
                PhysicalNicRegInfo item;
                item.guidUpper  = ToUpper(guidBuf);
                item.driverDesc = descBuf;
                item.infPath    = infBuf;
                item.isWifi     = (ifType == 71); // IF_TYPE_IEEE80211
                result.push_back(std::move(item));
            }
        }
        RegCloseKey(hSub);
    }
    RegCloseKey(hClass);
    return result;
}

void QueryTcpipInterfaceRegistry(const std::wstring& adapterGuid,
                                 bool& outStaticIp,
                                 bool& outStaticDns,
                                 std::wstring& outStaticDns1,
                                 std::wstring& outStaticDns2) {
    outStaticIp  = false;
    outStaticDns = false;
    outStaticDns1.clear();
    outStaticDns2.clear();

    std::wstring regPath = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\" + adapterGuid;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return;
    }

    DWORD enableDhcp = 1;
    DWORD cb = sizeof(enableDhcp);
    DWORD type = 0;
    if (RegQueryValueExW(hKey, L"EnableDHCP", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&enableDhcp), &cb) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        outStaticIp = (enableDhcp == 0);
    }

    wchar_t nsBuf[512]{};
    cb = sizeof(nsBuf);
    if (RegQueryValueExW(hKey, L"NameServer", nullptr, &type,
                         reinterpret_cast<LPBYTE>(nsBuf), &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        std::wstring ns = Trim(nsBuf);
        if (!ns.empty()) {
            outStaticDns = true;
            // NameServer may be separated by ',' or ' '
            size_t sep = ns.find_first_of(L", ");
            if (sep == std::wstring::npos) {
                outStaticDns1 = ns;
            } else {
                outStaticDns1 = Trim(ns.substr(0, sep));
                size_t nextStart = ns.find_first_not_of(L", ", sep);
                if (nextStart != std::wstring::npos) {
                    size_t sep2 = ns.find_first_of(L", ", nextStart);
                    outStaticDns2 = Trim(sep2 == std::wstring::npos
                                         ? ns.substr(nextStart)
                                         : ns.substr(nextStart, sep2 - nextStart));
                }
            }
        }
    }
    RegCloseKey(hKey);
}

// Dynamically query connected Wi-Fi profile name via wlanapi.dll (safe even on desktops without WLAN service)
std::map<std::wstring, std::wstring> QueryConnectedWifiProfilesByGuid() {
    std::map<std::wstring, std::wstring> guidToProfile;
    HMODULE hWlan = LoadLibraryW(L"wlanapi.dll");
    if (!hWlan) return guidToProfile;

    using FnWlanOpenHandle     = DWORD(WINAPI*)(DWORD, PVOID, PDWORD, PHANDLE);
    using FnWlanEnumInterfaces = DWORD(WINAPI*)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*);
    using FnWlanQueryInterface = DWORD(WINAPI*)(HANDLE, const GUID*, WLAN_INTF_OPCODE, PVOID, PDWORD, PVOID*, PWLAN_OPCODE_VALUE_TYPE);
    using FnWlanFreeMemory     = VOID(WINAPI*)(PVOID);
    using FnWlanCloseHandle    = DWORD(WINAPI*)(HANDLE, PVOID);

    auto pOpen  = reinterpret_cast<FnWlanOpenHandle>(reinterpret_cast<void*>(GetProcAddress(hWlan, "WlanOpenHandle")));
    auto pEnum  = reinterpret_cast<FnWlanEnumInterfaces>(reinterpret_cast<void*>(GetProcAddress(hWlan, "WlanEnumInterfaces")));
    auto pQuery = reinterpret_cast<FnWlanQueryInterface>(reinterpret_cast<void*>(GetProcAddress(hWlan, "WlanQueryInterface")));
    auto pFree  = reinterpret_cast<FnWlanFreeMemory>(reinterpret_cast<void*>(GetProcAddress(hWlan, "WlanFreeMemory")));
    auto pClose = reinterpret_cast<FnWlanCloseHandle>(reinterpret_cast<void*>(GetProcAddress(hWlan, "WlanCloseHandle")));

    if (pOpen && pEnum && pQuery && pFree && pClose) {
        DWORD negVer = 0;
        HANDLE hClient = nullptr;
        if (pOpen(2, nullptr, &negVer, &hClient) == ERROR_SUCCESS && hClient) {
            PWLAN_INTERFACE_INFO_LIST pIfList = nullptr;
            if (pEnum(hClient, nullptr, &pIfList) == ERROR_SUCCESS && pIfList) {
                for (DWORD i = 0; i < pIfList->dwNumberOfItems; ++i) {
                    const WLAN_INTERFACE_INFO& ifInfo = pIfList->InterfaceInfo[i];
                    if (ifInfo.isState == wlan_interface_state_connected) {
                        DWORD dataSize = 0;
                        PWLAN_CONNECTION_ATTRIBUTES pConn = nullptr;
                        if (pQuery(hClient, &ifInfo.InterfaceGuid, wlan_intf_opcode_current_connection,
                                   nullptr, &dataSize, reinterpret_cast<PVOID*>(&pConn), nullptr) == ERROR_SUCCESS &&
                            pConn) {
                            wchar_t guidStr[64]{};
                            const GUID& g = ifInfo.InterfaceGuid;
                            swprintf_s(guidStr,
                                       L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                                       g.Data1, g.Data2, g.Data3,
                                       g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                                       g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
                            guidToProfile[ToUpper(guidStr)] = pConn->strProfileName;
                            pFree(pConn);
                        }
                    }
                }
                pFree(pIfList);
            }
            pClose(hClient, nullptr);
        }
    }
    FreeLibrary(hWlan);
    return guidToProfile;
}

struct ExecOutput {
    DWORD exitCode = static_cast<DWORD>(-1);
    std::wstring text;
};

ExecOutput RunHiddenProcess(const std::wstring& exeName,
                            const std::wstring& args,
                            const LogCallback& logCb,
                            bool streamProgress = false) {
    ExecOutput out{};
    wchar_t sysDir[MAX_PATH]{};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    std::wstring exePath = JoinPath(sysDir, exeName);
    if (GetFileAttributesW(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        exePath = exeName;
    }

    std::wstring cmdLine = L"\"" + exePath + L"\" " + args;
    if (logCb) {
        logCb(L"> " + exeName + L" " + args);
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr;
    HANDLE hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        out.text = L"파이프 생성 실패";
        if (logCb) logCb(out.text);
        return out;
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
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);

    if (!ok) {
        CloseHandle(hRead);
        out.text = L"프로세스 시작 실패: " + exeName;
        if (logCb) logCb(out.text);
        return out;
    }

    std::string allBytes;
    std::string lineBuf;
    std::wstring lastPctLogged;
    char chunk[1024];
    DWORD bytesRead = 0;

    while (ReadFile(hRead, chunk, sizeof(chunk), &bytesRead, nullptr) && bytesRead > 0) {
        allBytes.append(chunk, bytesRead);
        if (streamProgress && logCb) {
            for (DWORD i = 0; i < bytesRead; ++i) {
                char c = chunk[i];
                if (c == '\r' || c == '\n') {
                    std::wstring line = Trim(DecodeConsoleBytes(lineBuf));
                    lineBuf.clear();
                    if (!line.empty()) {
                        // Throttle repetitive progress bars unless percentage changed cleanly
                        if (line.front() == L'[') {
                            // Extract e.g. "20.0%" or "50.0%" or "100.0%"
                            size_t pctPos = line.find(L'%');
                            if (pctPos != std::wstring::npos && pctPos >= 2) {
                                size_t st = line.find_last_not_of(L"0123456789.", pctPos - 1);
                                std::wstring numStr = line.substr(st == std::wstring::npos ? 0 : st + 1,
                                                                  pctPos - (st == std::wstring::npos ? 0 : st + 1));
                                int pctInt = _wtoi(numStr.c_str());
                                // Log every 20% milestone
                                int bucket = (pctInt / 20) * 20;
                                std::wstring bucketKey = std::to_wstring(bucket);
                                if (pctInt > 0 && bucketKey != lastPctLogged) {
                                    lastPctLogged = bucketKey;
                                    logCb(L"   진행률: " + numStr + L"%");
                                }
                            }
                        } else {
                            logCb(L"   " + line);
                        }
                    }
                } else {
                    lineBuf.push_back(c);
                }
            }
        }
    }
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &out.exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    out.text = Trim(DecodeConsoleBytes(allBytes));
    if (!streamProgress && logCb && !out.text.empty()) {
        logCb(out.text);
    }
    return out;
}

void EnsureUnicodeIni(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        const BYTE bom[2] = { 0xFF, 0xFE };
        DWORD written = 0;
        WriteFile(hFile, bom, sizeof(bom), &written, nullptr);
        CloseHandle(hFile);
    }
}

bool WriteAsciiFile(const std::wstring& path, const std::string& content) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
    }
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(hFile);
    return ok != FALSE;
}

bool AppendAsciiFile(const std::wstring& path, const std::string& content) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
    }
    HANDLE hFile = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(hFile);
    return ok != FALSE;
}

enum class TextEncoding { AsciiOrUtf8NoBom, Utf8Bom, Utf16Le };

bool ReadTextAutoEncoding(const std::wstring& path, std::wstring& outText, TextEncoding& outEnc) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(hFile, nullptr);
    if (size == INVALID_FILE_SIZE) {
        CloseHandle(hFile);
        return false;
    }
    std::vector<BYTE> raw(size);
    DWORD readBytes = 0;
    if (size > 0) {
        ReadFile(hFile, raw.data(), size, &readBytes, nullptr);
    }
    CloseHandle(hFile);

    if (readBytes >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        outEnc = TextEncoding::Utf16Le;
        size_t wchars = (readBytes - 2) / sizeof(wchar_t);
        outText.assign(reinterpret_cast<const wchar_t*>(raw.data() + 2), wchars);
        return true;
    }
    if (readBytes >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        outEnc = TextEncoding::Utf8Bom;
        std::string s(reinterpret_cast<const char*>(raw.data() + 3), readBytes - 3);
        outText = DecodeConsoleBytes(s);
        return true;
    }
    outEnc = TextEncoding::AsciiOrUtf8NoBom;
    std::string s(reinterpret_cast<const char*>(raw.data()), readBytes);
    outText = DecodeConsoleBytes(s);
    return true;
}

bool WriteTextWithEncoding(const std::wstring& path, const std::wstring& text, TextEncoding enc) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
    }
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = TRUE;
    if (enc == TextEncoding::Utf16Le) {
        const BYTE bom[2] = { 0xFF, 0xFE };
        WriteFile(hFile, bom, 2, &written, nullptr);
        ok = WriteFile(hFile, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &written, nullptr);
    } else if (enc == TextEncoding::Utf8Bom) {
        const BYTE bom[3] = { 0xEF, 0xBB, 0xBF };
        WriteFile(hFile, bom, 3, &written, nullptr);
        std::string u8 = WideToUtf8(text);
        ok = WriteFile(hFile, u8.data(), static_cast<DWORD>(u8.size()), &written, nullptr);
    } else {
        std::string acp = WideToOemOrAcp(text, CP_ACP);
        ok = WriteFile(hFile, acp.data(), static_cast<DWORD>(acp.size()), &written, nullptr);
    }
    CloseHandle(hFile);
    return ok != FALSE;
}

} // namespace

DetectedNetworkSummary DetectActiveWindowsNetwork() {
    DetectedNetworkSummary summary{};
    auto physNics = EnumeratePhysicalNicsFromRegistry();
    auto wifiProfiles = QueryConnectedWifiProfilesByGuid();

    std::set<std::wstring> seenInfs;
    std::map<std::wstring, PhysicalNicRegInfo> physByGuid;
    for (const auto& pn : physNics) {
        physByGuid[pn.guidUpper] = pn;
        std::wstring lowInf = ToLower(pn.infPath);
        if (lowInf.rfind(L"oem", 0) == 0 && lowInf.size() > 4) {
            if (seenInfs.insert(lowInf).second) {
                summary.physicalOemInfs.push_back(pn.infPath);
            }
        }
    }

    ULONG bufLen = 32768;
    std::vector<BYTE> buffer(bufLen);
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_INCLUDE_ALL_INTERFACES;
    DWORD ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                     reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufLen);
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                   reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &bufLen);
    }
    if (ret != NO_ERROR) {
        summary.shortBannerText = L"네트워크 어댑터 조회 실패";
        return summary;
    }

    // First pass: map MAC addresses of physical NICs so Hyper-V External vSwitches inherit hardware info
    std::map<std::wstring, PhysicalNicRegInfo> physByMac;
    for (auto* aa = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); aa; aa = aa->Next) {
        std::wstring guidUp = ToUpper(AnsiToWide(aa->AdapterName));
        auto it = physByGuid.find(guidUp);
        if (it != physByGuid.end() && aa->PhysicalAddressLength >= 6) {
            std::wstring mac = FormatMacDash(aa->PhysicalAddress, aa->PhysicalAddressLength);
            if (!mac.empty()) {
                physByMac[mac] = it->second;
            }
        }
    }

    // Second pass: collect active IP-bearing interfaces matched to physical hardware
    for (auto* aa = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); aa; aa = aa->Next) {
        if (aa->OperStatus != IfOperStatusUp) continue;
        if (aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK || aa->IfType == IF_TYPE_TUNNEL) continue;
        if (aa->PhysicalAddressLength < 6) continue;

        std::wstring mac = FormatMacDash(aa->PhysicalAddress, aa->PhysicalAddressLength);
        std::wstring guidUp = ToUpper(AnsiToWide(aa->AdapterName));

        // Match either directly by GUID or via shared MAC (e.g., Hyper-V External Virtual Switch)
        const PhysicalNicRegInfo* matchedPhys = nullptr;
        auto itGuid = physByGuid.find(guidUp);
        if (itGuid != physByGuid.end()) {
            matchedPhys = &itGuid->second;
        } else {
            auto itMac = physByMac.find(mac);
            if (itMac != physByMac.end()) {
                matchedPhys = &itMac->second;
            }
        }
        if (!matchedPhys) continue; // Ignore pure internal virtual switches (WSL/Default Switch)

        // Extract usable IPv4 address
        std::wstring ipv4, mask;
        for (auto* ua = aa->FirstUnicastAddress; ua; ua = ua->Next) {
            std::wstring candidate = FormatIpv4Sockaddr(ua->Address.lpSockaddr);
            if (IsUsableIpv4(candidate)) {
                ipv4 = candidate;
                mask = PrefixLengthToMask(ua->OnLinkPrefixLength);
                break;
            }
        }
        if (ipv4.empty()) continue;

        std::wstring gw;
        for (auto* ga = aa->FirstGatewayAddress; ga; ga = ga->Next) {
            std::wstring cgw = FormatIpv4Sockaddr(ga->Address.lpSockaddr);
            if (IsUsableIpv4(cgw)) {
                gw = cgw;
                break;
            }
        }

        AdapterConfig cfg{};
        cfg.valid        = true;
        cfg.isWifi       = matchedPhys->isWifi || (aa->IfType == IF_TYPE_IEEE80211);
        cfg.friendlyName = aa->FriendlyName ? aa->FriendlyName : L"";
        cfg.hardwareDesc = matchedPhys->driverDesc.empty()
                           ? (aa->Description ? aa->Description : L"")
                           : matchedPhys->driverDesc;
        cfg.adapterGuid  = guidUp;
        cfg.macDash      = mac;
        cfg.infName      = matchedPhys->infPath;
        cfg.ipv4         = ipv4;
        cfg.subnetMask   = mask;
        cfg.gateway      = gw;

        QueryTcpipInterfaceRegistry(guidUp, cfg.staticIp, cfg.staticDns, cfg.dns1, cfg.dns2);

        // If static DNS wasn't explicitly set in registry, capture current active DNS for reference
        if (cfg.dns1.empty()) {
            for (auto* da = aa->FirstDnsServerAddress; da; da = da->Next) {
                std::wstring cdns = FormatIpv4Sockaddr(da->Address.lpSockaddr);
                if (IsUsableIpv4(cdns)) {
                    if (cfg.dns1.empty()) cfg.dns1 = cdns;
                    else if (cfg.dns2.empty() && cdns != cfg.dns1) cfg.dns2 = cdns;
                }
            }
        }

        // Check Wi-Fi profile by physical NIC GUID or current adapter GUID
        auto itWifi = wifiProfiles.find(matchedPhys->guidUpper);
        if (itWifi == wifiProfiles.end()) itWifi = wifiProfiles.find(guidUp);
        if (itWifi != wifiProfiles.end()) {
            cfg.wifiProfile = itWifi->second;
            cfg.isWifi      = true;
        }

        summary.adapters.push_back(std::move(cfg));
    }

    // Prioritize adapters that have a default gateway (actual internet/LAN route)
    std::stable_sort(summary.adapters.begin(), summary.adapters.end(),
                     [](const AdapterConfig& a, const AdapterConfig& b) {
                         return (!a.gateway.empty() && b.gateway.empty());
                     });

    summary.foundAny = !summary.adapters.empty();
    if (summary.foundAny) {
        const auto& pri = summary.adapters.front();
        std::wstring kind = pri.isWifi
                            ? (L"Wi-Fi" + (pri.wifiProfile.empty() ? L"" : (L":" + pri.wifiProfile)))
                            : L"유선";
        std::wstring mode = pri.staticIp ? L"고정IP " : L"DHCP ";
        summary.shortBannerText = L"[" + kind + L"] " + pri.hardwareDesc + L" (" +
                                  mode + pri.ipv4 +
                                  (pri.infName.empty() ? L"" : (L", " + pri.infName)) + L")";
    } else {
        summary.shortBannerText = L"활성 물리 네트워크 연결 없음";
    }

    return summary;
}

std::wstring ComputeFileSignature(const std::wstring& fullPath) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(fullPath.c_str(), GetFileExInfoStandard, &fad)) {
        return L"";
    }
    ULONGLONG sz = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    wchar_t buf[128];
    swprintf_s(buf, L"%llu:%08lX:%08lX",
               sz, fad.ftLastWriteTime.dwHighDateTime, fad.ftLastWriteTime.dwLowDateTime);
    return buf;
}

namespace {

#pragma pack(push, 1)
struct WimResHdrDiskShort {
    uint64_t sizeAndFlags; // Low 56 bits = size on disk, High 8 bits = flags (0x04 = compressed)
    uint64_t offset;       // File offset in bytes
    uint64_t originalSize; // Uncompressed size in bytes
};

struct WimHeaderV1Packed {
    char               imageTag[8];       // "MSWIM\0\0\0"
    uint32_t           cbSize;            // 208 (0xD0)
    uint32_t           dwVersion;
    uint32_t           dwFlags;           // 0x00080000 = FLAG_HEADER_COMPRESS_LZMS (ESD)
    uint32_t           dwCompressionSize;
    BYTE               gWIMGuid[16];
    uint16_t           usPartNumber;
    uint16_t           usTotalParts;
    uint32_t           dwImageCount;
    WimResHdrDiskShort rhOffsetTable;
    WimResHdrDiskShort rhXmlData;
    WimResHdrDiskShort rhBootMetadata;
    uint32_t           dwBootIndex;
    WimResHdrDiskShort rhIntegrity;
    BYTE               bUnused[60];
};
#pragma pack(pop)

static_assert(sizeof(WimHeaderV1Packed) == 208, "WIMHEADER_V1_PACKED must be 208 bytes");

std::wstring ExtractXmlTagValue(const std::wstring& xml, const std::wstring& tagUpper) {
    std::wstring upXml = ToUpper(xml);
    std::wstring openTag  = L"<" + tagUpper + L">";
    std::wstring closeTag = L"</" + tagUpper + L">";
    size_t st = upXml.find(openTag);
    if (st == std::wstring::npos) return L"";
    st += openTag.size();
    size_t ed = upXml.find(closeTag, st);
    if (ed == std::wstring::npos || ed < st) return L"";
    return Trim(xml.substr(st, ed - st));
}

bool TrySetWimBootIndexViaWimgapi(const std::wstring& wimFullPath, uint32_t targetIndex) {
    DWORD attr = GetFileAttributesW(wimFullPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    if (attr & FILE_ATTRIBUTE_READONLY) {
        SetFileAttributesW(wimFullPath.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
    }

    HMODULE hWimDll = LoadLibraryW(L"wimgapi.dll");
    if (!hWimDll) return false;

    using FnWIMCreateFile     = HANDLE(WINAPI*)(PCWSTR, DWORD, DWORD, DWORD, DWORD, PDWORD);
    using FnWIMSetTempPath    = BOOL(WINAPI*)(HANDLE, PCWSTR);
    using FnWIMSetBootImage   = BOOL(WINAPI*)(HANDLE, DWORD);
    using FnWIMCloseHandle    = BOOL(WINAPI*)(HANDLE);

    auto pCreate  = reinterpret_cast<FnWIMCreateFile>(reinterpret_cast<void*>(GetProcAddress(hWimDll, "WIMCreateFile")));
    auto pSetTemp = reinterpret_cast<FnWIMSetTempPath>(reinterpret_cast<void*>(GetProcAddress(hWimDll, "WIMSetTemporaryPath")));
    auto pSetBoot = reinterpret_cast<FnWIMSetBootImage>(reinterpret_cast<void*>(GetProcAddress(hWimDll, "WIMSetBootImage")));
    auto pClose   = reinterpret_cast<FnWIMCloseHandle>(reinterpret_cast<void*>(GetProcAddress(hWimDll, "WIMCloseHandle")));

    bool ok = false;
    if (pCreate && pSetBoot && pClose) {
        DWORD cr = 0;
        HANDLE hWim = pCreate(wimFullPath.c_str(), GENERIC_READ | GENERIC_WRITE, OPEN_EXISTING, 0, 0, &cr);
        if (hWim && hWim != INVALID_HANDLE_VALUE) {
            wchar_t tmpDir[MAX_PATH]{};
            if (pSetTemp && GetTempPathW(MAX_PATH, tmpDir) > 0) {
                pSetTemp(hWim, tmpDir);
            }
            ok = (pSetBoot(hWim, targetIndex) != FALSE);
            pClose(hWim);
        }
    }
    FreeLibrary(hWimDll);
    return ok;
}

} // namespace

WimInspectionInfo InspectWimFile(const std::wstring& wimFullPath) {
    WimInspectionInfo info{};
    HANDLE hFile = CreateFileW(wimFullPath.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return info;

    info.fileOpened = true;
    WimHeaderV1Packed hdr{};
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, &hdr, sizeof(hdr), &bytesRead, nullptr) || bytesRead < sizeof(hdr)) {
        CloseHandle(hFile);
        return info;
    }

    const char kMagic[8] = { 'M', 'S', 'W', 'I', 'M', '\0', '\0', '\0' };
    if (std::memcmp(hdr.imageTag, kMagic, 8) != 0) {
        CloseHandle(hFile);
        return info;
    }

    info.validWimMagic = true;
    info.isLzmsEsd     = (hdr.dwFlags & 0x00080000u) != 0; // FLAG_HEADER_COMPRESS_LZMS
    info.isSplitSwm    = (hdr.usTotalParts > 1);
    info.imageCount    = hdr.dwImageCount;
    info.bootIndex     = hdr.dwBootIndex;

    // Parse uncompressed UTF-16LE XML manifest if present
    uint64_t xmlSize  = hdr.rhXmlData.sizeAndFlags & 0x00FFFFFFFFFFFFFFULL;
    uint8_t  xmlFlags = static_cast<uint8_t>(hdr.rhXmlData.sizeAndFlags >> 56);
    if ((xmlFlags & 0x04) == 0 && xmlSize >= 4 && xmlSize <= (4u * 1024u * 1024u)) {
        LARGE_INTEGER seekPos{};
        seekPos.QuadPart = static_cast<LONGLONG>(hdr.rhXmlData.offset);
        if (SetFilePointerEx(hFile, seekPos, nullptr, FILE_BEGIN)) {
            std::vector<BYTE> xmlBytes(static_cast<size_t>(xmlSize));
            DWORD xmlRead = 0;
            if (ReadFile(hFile, xmlBytes.data(), static_cast<DWORD>(xmlSize), &xmlRead, nullptr) && xmlRead >= 4) {
                size_t byteOffset = 0;
                if (xmlBytes[0] == 0xFF && xmlBytes[1] == 0xFE) {
                    byteOffset = 2;
                }
                std::wstring xmlStr(reinterpret_cast<const wchar_t*>(xmlBytes.data() + byteOffset),
                                    (xmlRead - byteOffset) / sizeof(wchar_t));

                uint32_t targetIdx = (info.bootIndex > 0) ? info.bootIndex
                                                          : ((info.imageCount > 0) ? info.imageCount : 1u);
                std::wstring upXml = ToUpper(xmlStr);
                std::wstring idxMarker = L"<IMAGE INDEX=\"" + std::to_wstring(targetIdx) + L"\"";
                size_t imgStart = upXml.find(idxMarker);
                if (imgStart == std::wstring::npos) {
                    imgStart = upXml.find(L"<IMAGE ");
                }

                std::wstring slice = xmlStr;
                if (imgStart != std::wstring::npos) {
                    size_t imgEnd = upXml.find(L"</IMAGE>", imgStart);
                    slice = (imgEnd != std::wstring::npos)
                            ? xmlStr.substr(imgStart, imgEnd - imgStart)
                            : xmlStr.substr(imgStart);
                }

                info.imageName = ExtractXmlTagValue(slice, L"NAME");
                std::wstring archStr = ExtractXmlTagValue(slice, L"ARCH");
                if (!archStr.empty()) {
                    info.arch = _wtoi(archStr.c_str());
                }
                std::wstring buildStr = ExtractXmlTagValue(slice, L"BUILD");
                if (!buildStr.empty()) {
                    info.osBuild = _wtoi(buildStr.c_str());
                }
                std::wstring sysRoot = ExtractXmlTagValue(slice, L"SYSTEMROOT");
                if (!sysRoot.empty()) {
                    if (sysRoot.front() != L'\\') sysRoot = L"\\" + sysRoot;
                    info.systemRoot = sysRoot;
                }
            }
        }
    }

    CloseHandle(hFile);
    return info;
}

bool ConvertEsdWimToBootableWim(const std::wstring& wimFullPath,
                                uint32_t sourceIndex,
                                const LogCallback& logCb,
                                std::wstring& outNewWimSig) {
    outNewWimSig.clear();
    DWORD attr = GetFileAttributesW(wimFullPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    if (attr & FILE_ATTRIBUTE_READONLY) {
        SetFileAttributesW(wimFullPath.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
    }

    if (sourceIndex == 0) sourceIndex = 1;
    std::wstring tmpWim = wimFullPath + L".bootfix.tmp";
    std::wstring bakWim = wimFullPath + L".esd.bak";
    DeleteFileW(tmpWim.c_str());

    if (logCb) {
        logCb(L"--- WIM 이미지를 RAMDisk 부팅 표준 포맷(LZX + Bootable)으로 변환 시작 ---");
    }

    std::wstring exportArgs =
        L"/English /Export-Image /SourceImageFile:\"" + wimFullPath +
        L"\" /SourceIndex:" + std::to_wstring(sourceIndex) +
        L" /DestinationImageFile:\"" + tmpWim +
        L"\" /Compress:max /Bootable";

    ExecOutput er = RunHiddenProcess(L"dism.exe", exportArgs, logCb, true);
    if (er.exitCode != 0 || GetFileAttributesW(tmpWim.c_str()) == INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(tmpWim.c_str());
        if (logCb) logCb(L"오류: DISM 이미지 변환(/Export-Image)에 실패했습니다.");
        return false;
    }

    DeleteFileW(bakWim.c_str());
    if (!MoveFileExW(wimFullPath.c_str(), bakWim.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmpWim.c_str());
        if (logCb) logCb(L"오류: 원본 WIM 파일을 교체할 수 없습니다 (다른 프로그램이 사용 중인지 확인하세요).");
        return false;
    }

    if (!MoveFileExW(tmpWim.c_str(), wimFullPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        // Restore backup on failure
        MoveFileExW(bakWim.c_str(), wimFullPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (logCb) logCb(L"오류: 변환된 WIM 파일 이름 변경에 실패하여 원본을 복원했습니다.");
        return false;
    }

    DeleteFileW(bakWim.c_str());
    outNewWimSig = ComputeFileSignature(wimFullPath);
    if (logCb) logCb(L"★ RAMDisk 부팅 표준 WIM 변환 및 Bootable 플래그 설정 완료!");
    return true;
}

bool EnsureWimBootableFlag(const std::wstring& wimFullPath,
                           uint32_t targetIndex,
                           const LogCallback& logCb) {
    if (targetIndex == 0) targetIndex = 1;
    if (TrySetWimBootIndexViaWimgapi(wimFullPath, targetIndex)) {
        WimInspectionInfo check = InspectWimFile(wimFullPath);
        if (check.bootIndex == targetIndex) {
            if (logCb) {
                logCb(L"WIM 부팅 가능 플래그(BootIndex=" + std::to_wstring(targetIndex) + L") 자동 설정 완료.");
            }
            return true;
        }
    }

    std::wstring dummySig;
    return ConvertEsdWimToBootableWim(wimFullPath, targetIndex, logCb, dummySig);
}

std::wstring DetectPreferredLoaderPathInWim(const std::wstring& wimFullPath,
                                            uint32_t imageIndex,
                                            bool isUefi,
                                            const std::wstring& currentLoaderPath) {
    if (imageIndex == 0) imageIndex = 1;

    HMODULE hWimDll = LoadLibraryW(L"wimgapi.dll");
    if (!hWimDll) return L"";

    using FnWIMCreateFile       = HANDLE(WINAPI*)(PCWSTR, DWORD, DWORD, DWORD, DWORD, PDWORD);
    using FnWIMSetTempPath      = BOOL(WINAPI*)(HANDLE, PCWSTR);
    using FnWIMLoadImage        = HANDLE(WINAPI*)(HANDLE, DWORD);
    using FnWIMExtractImagePath = BOOL(WINAPI*)(HANDLE, PCWSTR, PCWSTR, DWORD);
    using FnWIMCloseHandle      = BOOL(WINAPI*)(HANDLE);

    auto pCreate  = reinterpret_cast<FnWIMCreateFile>(reinterpret_cast<void*>(GetProcAddress(hWimDll, "WIMCreateFile")));
    auto pSetTemp = reinterpret_cast<FnWIMSetTempPath>(reinterpret_cast<void*>(GetProcAddress(hWimDll, "WIMSetTemporaryPath")));
    auto pLoad    = reinterpret_cast<FnWIMLoadImage>(reinterpret_cast<void*>(GetProcAddress(hWimDll, "WIMLoadImage")));
    auto pExtract = reinterpret_cast<FnWIMExtractImagePath>(reinterpret_cast<void*>(GetProcAddress(hWimDll, "WIMExtractImagePath")));
    auto pClose   = reinterpret_cast<FnWIMCloseHandle>(reinterpret_cast<void*>(GetProcAddress(hWimDll, "WIMCloseHandle")));

    std::wstring detected;
    if (pCreate && pLoad && pExtract && pClose) {
        DWORD cr = 0;
        HANDLE hWim = pCreate(wimFullPath.c_str(), GENERIC_READ, OPEN_EXISTING, 0, 0, &cr);
        if (hWim && hWim != INVALID_HANDLE_VALUE) {
            wchar_t tmpDir[MAX_PATH]{};
            if (GetTempPathW(MAX_PATH, tmpDir) > 0) {
                if (pSetTemp) pSetTemp(hWim, tmpDir);

                HANDLE hImg = pLoad(hWim, imageIndex);
                if (hImg && hImg != INVALID_HANDLE_VALUE) {
                    std::wstring probeDst = JoinPath(tmpDir, L"otpe_loader_probe.tmp");
                    auto probePath = [&](const std::wstring& relPath) -> bool {
                        DeleteFileW(probeDst.c_str());
                        BOOL ok = pExtract(hImg, relPath.c_str(), probeDst.c_str(), 0);
                        if (!ok && !relPath.empty() && relPath.front() == L'\\') {
                            ok = pExtract(hImg, relPath.substr(1).c_str(), probeDst.c_str(), 0);
                        }
                        bool exists = ok && (GetFileAttributesW(probeDst.c_str()) != INVALID_FILE_ATTRIBUTES);
                        DeleteFileW(probeDst.c_str());
                        return exists;
                    };

                    const std::wstring stdPath  = isUefi ? L"\\Windows\\System32\\winload.efi"
                                                         : L"\\Windows\\System32\\winload.exe";
                    const std::wstring bootPath = isUefi ? L"\\Windows\\System32\\Boot\\winload.efi"
                                                         : L"\\Windows\\System32\\Boot\\winload.exe";

                    bool curOk = !currentLoaderPath.empty() && probePath(currentLoaderPath);
                    if (!curOk) {
                        if (probePath(stdPath)) {
                            detected = isUefi ? L"\\windows\\system32\\winload.efi"
                                              : L"\\windows\\system32\\winload.exe";
                        } else if (probePath(bootPath)) {
                            detected = isUefi ? L"\\windows\\system32\\boot\\winload.efi"
                                              : L"\\windows\\system32\\boot\\winload.exe";
                        }
                    }
                    pClose(hImg);
                }
            }
            pClose(hWim);
        }
    }

    FreeLibrary(hWimDll);
    return detected;
}

bool IsWimUnattendedPatched(const std::wstring& wimFullPath, uint32_t imageIndex) {
    if (wimFullPath.empty()) return false;
    std::wstring sig = ComputeFileSignature(wimFullPath);
    if (sig.empty()) return false;

    static std::map<std::wstring, std::pair<std::wstring, bool>> s_patchCache;
    std::wstring key = ToLower(wimFullPath);
    auto it = s_patchCache.find(key);
    if (it != s_patchCache.end() && it->second.first == sig) {
        return it->second.second;
    }

    if (imageIndex == 0) {
        WimInspectionInfo wi = InspectWimFile(wimFullPath);
        imageIndex = (wi.bootIndex > 0) ? wi.bootIndex : 1u;
    }

    bool patched = false;
    HMODULE hWimDll = LoadLibraryW(L"wimgapi.dll");
    if (hWimDll) {
        using FnWIMCreateFile       = HANDLE(WINAPI*)(PCWSTR, DWORD, DWORD, DWORD, DWORD, PDWORD);
        using FnWIMSetTempPath      = BOOL(WINAPI*)(HANDLE, PCWSTR);
        using FnWIMLoadImage        = HANDLE(WINAPI*)(HANDLE, DWORD);
        using FnWIMExtractImagePath = BOOL(WINAPI*)(HANDLE, PCWSTR, PCWSTR, DWORD);
        using FnWIMCloseHandle      = BOOL(WINAPI*)(HANDLE);

        auto pCreate  = reinterpret_cast<FnWIMCreateFile>(reinterpret_cast<void*>(GetProcAddress(hWimDll, "WIMCreateFile")));
        auto pSetTemp = reinterpret_cast<FnWIMSetTempPath>(reinterpret_cast<void*>(GetProcAddress(hWimDll, "WIMSetTemporaryPath")));
        auto pLoad    = reinterpret_cast<FnWIMLoadImage>(reinterpret_cast<void*>(GetProcAddress(hWimDll, "WIMLoadImage")));
        auto pExtract = reinterpret_cast<FnWIMExtractImagePath>(reinterpret_cast<void*>(GetProcAddress(hWimDll, "WIMExtractImagePath")));
        auto pClose   = reinterpret_cast<FnWIMCloseHandle>(reinterpret_cast<void*>(GetProcAddress(hWimDll, "WIMCloseHandle")));

        if (pCreate && pLoad && pExtract && pClose) {
            DWORD cr = 0;
            HANDLE hWim = pCreate(wimFullPath.c_str(), GENERIC_READ, OPEN_EXISTING, 0, 0, &cr);
            if (hWim && hWim != INVALID_HANDLE_VALUE) {
                wchar_t tmpDir[MAX_PATH]{};
                if (GetTempPathW(MAX_PATH, tmpDir) > 0) {
                    if (pSetTemp) pSetTemp(hWim, tmpDir);
                    HANDLE hImg = pLoad(hWim, imageIndex);
                    if (hImg && hImg != INVALID_HANDLE_VALUE) {
                        std::wstring probeDst = JoinPath(tmpDir, L"otpe_patch_probe.tmp");
                        DeleteFileW(probeDst.c_str());
                        BOOL ok = pExtract(hImg, L"\\Windows\\System32\\OneTimePE_WimPatch.ini", probeDst.c_str(), 0);
                        if (!ok) {
                            ok = pExtract(hImg, L"Windows\\System32\\OneTimePE_WimPatch.ini", probeDst.c_str(), 0);
                        }
                        if (ok && GetFileAttributesW(probeDst.c_str()) != INVALID_FILE_ATTRIBUTES) {
                            int ver = static_cast<int>(GetPrivateProfileIntW(L"WimPatch", L"Version", 0, probeDst.c_str()));
                            patched = (ver >= 3);
                        }
                        DeleteFileW(probeDst.c_str());
                        pClose(hImg);
                    }
                }
                pClose(hWim);
            }
        }
        FreeLibrary(hWimDll);
    }

    s_patchCache[key] = { sig, patched };
    return patched;
}

namespace {

bool EndsWithLower(const std::wstring& str, const std::wstring& suffixLower) {
    std::wstring low = ToLower(str);
    return low.size() >= suffixLower.size() &&
           low.compare(low.size() - suffixLower.size(), suffixLower.size(), suffixLower) == 0;
}

std::wstring ParentDirOf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    size_t sl = p.find_last_of(L"\\/");
    if (sl == std::wstring::npos) return L"";
    if (sl == 2 && p.size() >= 2 && p[1] == L':') return p.substr(0, 3);
    return p.substr(0, sl);
}

std::wstring FileNameOf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    size_t sl = p.find_last_of(L"\\/");
    return (sl == std::wstring::npos) ? p : p.substr(sl + 1);
}

std::wstring StemOf(const std::wstring& fileName) {
    size_t dot = fileName.find_last_of(L'.');
    return (dot == std::wstring::npos || dot == 0) ? fileName : fileName.substr(0, dot);
}

bool FileExistsRegular(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

bool DirExists(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

std::wstring EscapePsSingleQuoted(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 4);
    for (wchar_t c : s) {
        if (c == L'\'') out += L"''";
        else out.push_back(c);
    }
    return out;
}

bool QueryDeviceNumberForHandlePath(const std::wstring& devPath, STORAGE_DEVICE_NUMBER& outSdn) {
    HANDLE h = CreateFileW(devPath.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                              &outSdn, sizeof(outSdn), &ret, nullptr);
    CloseHandle(h);
    return ok != FALSE && ret >= sizeof(STORAGE_DEVICE_NUMBER);
}

std::wstring ResolvePhysicalCdromToDriveRoot(const std::wstring& physPath, DWORD newDrivesMask) {
    DWORD curDrives = GetLogicalDrives();

    // 1. Match via QueryDosDeviceW NT symlink target (e.g. "CDROM0" -> "\Device\CdRom0" == "E:")
    std::wstring dosDevName = physPath;
    if (dosDevName.rfind(L"\\\\.\\", 0) == 0) {
        dosDevName = dosDevName.substr(4);
    }
    wchar_t physNtTarget[MAX_PATH * 2]{};
    bool hasPhysNt = !dosDevName.empty() &&
                     (QueryDosDeviceW(dosDevName.c_str(), physNtTarget, _countof(physNtTarget)) > 0);
    if (!hasPhysNt && physPath.rfind(L"\\Device\\", 0) == 0) {
        wcsncpy_s(physNtTarget, _countof(physNtTarget), physPath.c_str(), _countof(physNtTarget) - 1);
        hasPhysNt = true;
    }

    STORAGE_DEVICE_NUMBER sdnPhys{};
    bool hasPhysSdn = !physPath.empty() && QueryDeviceNumberForHandlePath(physPath, sdnPhys);

    for (int bit = 0; bit < 26; ++bit) {
        if ((curDrives & (1u << bit)) == 0) continue;
        wchar_t drvColon[4] = { static_cast<wchar_t>(L'A' + bit), L':', L'\0', L'\0' };
        wchar_t drvRoot[4]  = { static_cast<wchar_t>(L'A' + bit), L':', L'\\', L'\0' };

        if (hasPhysNt) {
            wchar_t drvNtTarget[MAX_PATH * 2]{};
            if (QueryDosDeviceW(drvColon, drvNtTarget, _countof(drvNtTarget)) > 0 &&
                _wcsicmp(drvNtTarget, physNtTarget) == 0) {
                return drvRoot;
            }
        }
        if (hasPhysSdn) {
            STORAGE_DEVICE_NUMBER sdnDrv{};
            if (QueryDeviceNumberForHandlePath(std::wstring(L"\\\\.\\") + drvColon, sdnDrv) &&
                sdnDrv.DeviceType == sdnPhys.DeviceType &&
                sdnDrv.DeviceNumber == sdnPhys.DeviceNumber) {
                return drvRoot;
            }
        }
    }

    // 2. Fallback: newly appeared CD-ROM drive bit
    if (newDrivesMask != 0) {
        for (int bit = 0; bit < 26; ++bit) {
            if ((newDrivesMask & (1u << bit)) != 0) {
                wchar_t drvRoot[4] = { static_cast<wchar_t>(L'A' + bit), L':', L'\\', L'\0' };
                if (GetDriveTypeW(drvRoot) == DRIVE_CDROM) {
                    return drvRoot;
                }
            }
        }
    }

    return L"";
}

std::wstring FormatBcdGuidBytes(const uint8_t* b) {
    GUID g{};
    std::memcpy(&g, b, 16);
    wchar_t buf[64]{};
    swprintf_s(buf, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               g.Data1, g.Data2, g.Data3,
               g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
               g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return ToLower(buf);
}

struct BcdElementVal {
    uint32_t             dataType = 0;
    std::vector<uint8_t> raw;

    std::wstring asSz() const {
        if ((dataType == REG_SZ || dataType == REG_EXPAND_SZ) && raw.size() >= 2) {
            std::wstring s(reinterpret_cast<const wchar_t*>(raw.data()), raw.size() / 2);
            while (!s.empty() && s.back() == L'\0') s.pop_back();
            return Trim(s);
        }
        return L"";
    }

    std::vector<std::wstring> asMultiSz() const {
        std::vector<std::wstring> out;
        if (dataType == REG_MULTI_SZ && raw.size() >= 2) {
            const auto* p = reinterpret_cast<const wchar_t*>(raw.data());
            size_t n = raw.size() / 2;
            std::wstring cur;
            for (size_t i = 0; i < n; ++i) {
                if (p[i] == L'\0') {
                    if (!cur.empty()) {
                        out.push_back(Trim(cur));
                        cur.clear();
                    }
                } else {
                    cur.push_back(p[i]);
                }
            }
            if (!cur.empty()) out.push_back(Trim(cur));
        }
        return out;
    }

    bool asBool(bool defVal = false) const {
        if (raw.empty()) return defVal;
        return raw[0] != 0;
    }

    uint64_t asUint64(uint64_t defVal = UINT64_MAX) const {
        if (raw.size() >= 8) {
            uint64_t v = 0;
            std::memcpy(&v, raw.data(), 8);
            return v;
        }
        if (raw.size() >= 4) {
            uint32_t v = 0;
            std::memcpy(&v, raw.data(), 4);
            return v;
        }
        return defVal;
    }
};

struct BcdObjectNode {
    std::wstring                          guid;
    uint32_t                              objType = 0;
    std::map<std::wstring, BcdElementVal> elements; // lowercase 8-hex -> value
};

// Parse BCD binary device element (11000001 / 21000001) for BM_DEV_RAMDISK (.wim / .esd) + SDI device GUID
bool ParseRamdiskDeviceBlob(const std::vector<uint8_t>& raw,
                            std::wstring& outSdiGuid,
                            std::wstring& outWimRelPath) {
    outSdiGuid.clear();
    outWimRelPath.clear();
    if (raw.size() < 40) return false;

    GUID g{};
    std::memcpy(&g, raw.data(), 16);
    if (g.Data1 != 0 || g.Data2 != 0 || g.Data3 != 0) {
        outSdiGuid = FormatBcdGuidBytes(raw.data());
    }

    if (raw.size() >= 0x54) {
        uint32_t outerType = 0;
        std::memcpy(&outerType, raw.data() + 0x20, 4);
        uint32_t innerSize = 0;
        std::memcpy(&innerSize, raw.data() + 0x4C, 4);
        size_t strOff = 0x44u + static_cast<size_t>(innerSize);
        if (outerType == 3 && innerSize >= 0x10 && strOff + 4 <= raw.size()) {
            const auto* wp = reinterpret_cast<const wchar_t*>(raw.data() + strOff);
            size_t maxChars = (raw.size() - strOff) / 2;
            size_t len = 0;
            while (len < maxChars && wp[len] != L'\0') ++len;
            if (len > 0) {
                outWimRelPath.assign(wp, len);
                for (auto& ch : outWimRelPath) {
                    if (ch == L'/') ch = L'\\';
                }
                if (outWimRelPath.front() != L'\\') outWimRelPath = L"\\" + outWimRelPath;
                return true;
            }
        }
    }

    // Safe UTF-16LE fallback scan for "\*.wim" or "\*.esd"
    for (size_t off = 32; off + 10 <= raw.size(); off += 2) {
        wchar_t ch = 0;
        std::memcpy(&ch, raw.data() + off, 2);
        if (ch == L'\\' || ch == L'/') {
            std::wstring cand;
            for (size_t j = off; j + 1 < raw.size(); j += 2) {
                wchar_t c2 = 0;
                std::memcpy(&c2, raw.data() + j, 2);
                if (c2 == L'\0') break;
                if (c2 < 32 || c2 == L'*' || c2 == L'?' || c2 == L'|') {
                    cand.clear();
                    break;
                }
                cand.push_back(c2 == L'/' ? L'\\' : c2);
            }
            if (cand.size() >= 5 && (EndsWithLower(cand, L".wim") || EndsWithLower(cand, L".esd"))) {
                outWimRelPath = cand;
                return true;
            }
        }
    }
    return false;
}

// Pure C++ read-only Windows Registry Hive ("regf") parser for BCD stores (needs zero privileges)
class RegHiveReader {
public:
    bool load(const std::wstring& filePath) {
        HANDLE h = CreateFileW(filePath.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER li{};
        if (!GetFileSizeEx(h, &li) || li.QuadPart < 0x1020 || li.QuadPart > (64LL * 1024LL * 1024LL)) {
            CloseHandle(h);
            return false;
        }
        buf_.resize(static_cast<size_t>(li.QuadPart));
        DWORD rd = 0;
        BOOL ok = ReadFile(h, buf_.data(), static_cast<DWORD>(buf_.size()), &rd, nullptr);
        CloseHandle(h);
        if (!ok || rd < 0x1020 || std::memcmp(buf_.data(), "regf", 4) != 0) {
            buf_.clear();
            return false;
        }
        std::memcpy(&rootOff_, buf_.data() + 0x24, 4);
        return true;
    }

    uint32_t rootOffset() const { return rootOff_; }

    bool readNk(uint32_t nkOff,
                std::wstring& outName,
                std::vector<uint32_t>& outSubkeyOffsets,
                std::map<std::wstring, BcdElementVal>& outValues) const {
        outName.clear();
        outSubkeyOffsets.clear();
        outValues.clear();

        uint32_t len = 0;
        const uint8_t* p = cellPayload(nkOff, len);
        if (!p || len < 0x4Cu || std::memcmp(p, "nk", 2) != 0) return false;

        uint16_t flags = 0;
        uint16_t nameLen = 0;
        uint32_t numSub = 0;
        uint32_t subListOff = 0xFFFFFFFFu;
        uint32_t numVal = 0;
        uint32_t valListOff = 0xFFFFFFFFu;

        std::memcpy(&flags, p + 0x02, 2);
        std::memcpy(&numSub, p + 0x14, 4);
        std::memcpy(&subListOff, p + 0x1C, 4);
        std::memcpy(&numVal, p + 0x24, 4);
        std::memcpy(&valListOff, p + 0x28, 4);
        std::memcpy(&nameLen, p + 0x48, 2);

        if (0x4Cu + static_cast<uint32_t>(nameLen) > len) return false;
        outName = decodeKeyOrValName(p + 0x4C, nameLen, (flags & 0x0020u) != 0);

        if (numSub > 0 && subListOff != 0xFFFFFFFFu) {
            collectSubkeys(subListOff, outSubkeyOffsets, 0);
        }

        if (numVal > 0 && valListOff != 0xFFFFFFFFu) {
            uint32_t vlLen = 0;
            const uint8_t* vl = cellPayload(valListOff, vlLen);
            if (vl) {
                for (uint32_t i = 0; i < numVal && (i + 1u) * 4u <= vlLen; ++i) {
                    uint32_t vkOff = 0;
                    std::memcpy(&vkOff, vl + i * 4u, 4);
                    uint32_t vkLen = 0;
                    const uint8_t* vk = cellPayload(vkOff, vkLen);
                    if (!vk || vkLen < 0x14u || std::memcmp(vk, "vk", 2) != 0) continue;

                    uint16_t vnLen = 0;
                    uint16_t vFlags = 0;
                    uint32_t rawDLen = 0;
                    uint32_t dOff = 0;
                    uint32_t dType = 0;
                    std::memcpy(&vnLen, vk + 0x02, 2);
                    std::memcpy(&rawDLen, vk + 0x04, 4);
                    std::memcpy(&dOff, vk + 0x08, 4);
                    std::memcpy(&dType, vk + 0x0C, 4);
                    std::memcpy(&vFlags, vk + 0x10, 2);
                    if (0x14u + static_cast<uint32_t>(vnLen) > vkLen) continue;

                    std::wstring vName = ToLower(decodeKeyOrValName(vk + 0x14, vnLen, (vFlags & 0x0001u) != 0));
                    BcdElementVal ev{};
                    ev.dataType = dType;
                    bool inlineData = (rawDLen & 0x80000000u) != 0;
                    uint32_t dLen   = rawDLen & 0x7FFFFFFFu;
                    if (inlineData) {
                        if (dLen > 4u) dLen = 4u;
                        ev.raw.resize(dLen);
                        if (dLen > 0) std::memcpy(ev.raw.data(), &dOff, dLen);
                    } else if (dLen > 0 && dLen <= (4u * 1024u * 1024u)) {
                        uint32_t dcLen = 0;
                        const uint8_t* dc = cellPayload(dOff, dcLen);
                        if (dc && dcLen > 0) {
                            uint32_t copyLen = std::min(dLen, dcLen);
                            ev.raw.assign(dc, dc + copyLen);
                        }
                    }
                    outValues[vName] = std::move(ev);
                }
            }
        }
        return true;
    }

private:
    std::vector<uint8_t> buf_;
    uint32_t             rootOff_ = 0;

    const uint8_t* cellPayload(uint32_t relOff, uint32_t& outPayloadLen) const {
        outPayloadLen = 0;
        if (relOff == 0xFFFFFFFFu) return nullptr;
        uint64_t pos = 0x1000ULL + static_cast<uint64_t>(relOff);
        if (pos + 4ULL > buf_.size()) return nullptr;
        int32_t rawSz = 0;
        std::memcpy(&rawSz, buf_.data() + pos, 4);
        int64_t absSz = (rawSz < 0) ? -static_cast<int64_t>(rawSz) : static_cast<int64_t>(rawSz);
        if (absSz <= 4 || pos + static_cast<uint64_t>(absSz) > buf_.size()) return nullptr;
        outPayloadLen = static_cast<uint32_t>(absSz - 4);
        return buf_.data() + pos + 4;
    }

    static std::wstring decodeKeyOrValName(const uint8_t* p, uint16_t byteLen, bool isAscii) {
        std::wstring s;
        if (isAscii) {
            s.reserve(byteLen);
            for (uint16_t i = 0; i < byteLen; ++i) {
                s.push_back(static_cast<wchar_t>(p[i]));
            }
        } else {
            s.reserve(byteLen / 2u);
            for (uint16_t i = 0; static_cast<uint32_t>(i) + 1u < byteLen; i = static_cast<uint16_t>(i + 2u)) {
                wchar_t wc = 0;
                std::memcpy(&wc, p + i, 2);
                s.push_back(wc);
            }
        }
        return s;
    }

    void collectSubkeys(uint32_t listOff, std::vector<uint32_t>& out, int depth) const {
        if (depth > 4) return;
        uint32_t len = 0;
        const uint8_t* p = cellPayload(listOff, len);
        if (!p || len < 4u) return;
        uint16_t count = 0;
        std::memcpy(&count, p + 2, 2);
        if (std::memcmp(p, "lf", 2) == 0 || std::memcmp(p, "lh", 2) == 0) {
            for (uint32_t i = 0; i < count && 4u + (i + 1u) * 8u <= len; ++i) {
                uint32_t o = 0;
                std::memcpy(&o, p + 4u + i * 8u, 4);
                out.push_back(o);
            }
        } else if (std::memcmp(p, "li", 2) == 0) {
            for (uint32_t i = 0; i < count && 4u + (i + 1u) * 4u <= len; ++i) {
                uint32_t o = 0;
                std::memcpy(&o, p + 4u + i * 4u, 4);
                out.push_back(o);
            }
        } else if (std::memcmp(p, "ri", 2) == 0) {
            for (uint32_t i = 0; i < count && 4u + (i + 1u) * 4u <= len; ++i) {
                uint32_t sl = 0;
                std::memcpy(&sl, p + 4u + i * 4u, 4);
                collectSubkeys(sl, out, depth + 1);
            }
        }
    }
};

void MergeInheritedBcdElements(const std::wstring& guidLower,
                               const std::map<std::wstring, BcdObjectNode>& allObjects,
                               std::map<std::wstring, BcdElementVal>& ioMerged,
                               std::set<std::wstring>& visited,
                               int depth) {
    if (depth > 6 || guidLower.empty() || visited.count(guidLower) > 0) return;
    visited.insert(guidLower);

    auto itObj = allObjects.find(guidLower);
    if (itObj == allObjects.end()) return;
    const BcdObjectNode& node = itObj->second;

    auto itInh = node.elements.find(L"14000006");
    if (itInh != node.elements.end()) {
        for (const auto& parentGuid : itInh->second.asMultiSz()) {
            MergeInheritedBcdElements(ToLower(parentGuid), allObjects, ioMerged, visited, depth + 1);
        }
    }

    for (const auto& kv : node.elements) {
        ioMerged[kv.first] = kv.second;
    }
}

std::wstring FindFallbackSdiOnMedia(const std::wstring& mediaRoot,
                                    const std::wstring& wimFullPath,
                                    std::wstring& outSdiRelPath) {
    outSdiRelPath.clear();

    // 1. Check same directory as WIM first (e.g., \SSTR\boot.sdi or \WEPE\WEPE.SDI)
    std::wstring wimDir  = ParentDirOf(wimFullPath);
    std::wstring wimStem = StemOf(FileNameOf(wimFullPath));
    if (!wimDir.empty()) {
        std::wstring sameStemSdi = JoinPath(wimDir, wimStem + L".sdi");
        if (FileExistsRegular(sameStemSdi)) {
            if (ToLower(sameStemSdi).rfind(ToLower(mediaRoot), 0) == 0) {
                outSdiRelPath = sameStemSdi.substr(mediaRoot.size());
                if (!outSdiRelPath.empty() && outSdiRelPath.front() != L'\\') outSdiRelPath = L"\\" + outSdiRelPath;
            }
            return sameStemSdi;
        }

        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW(JoinPath(wimDir, L"*.sdi").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            std::wstring picked;
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    std::wstring cand = JoinPath(wimDir, fd.cFileName);
                    if (_wcsicmp(fd.cFileName, L"boot.sdi") == 0 || picked.empty()) {
                        picked = cand;
                        if (_wcsicmp(fd.cFileName, L"boot.sdi") == 0) break;
                    }
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
            if (!picked.empty()) {
                if (ToLower(picked).rfind(ToLower(mediaRoot), 0) == 0) {
                    outSdiRelPath = picked.substr(mediaRoot.size());
                    if (!outSdiRelPath.empty() && outSdiRelPath.front() != L'\\') outSdiRelPath = L"\\" + outSdiRelPath;
                }
                return picked;
            }
        }
    }

    // 2. Standard media locations
    const wchar_t* relCandidates[] = {
        L"\\boot\\boot.sdi",
        L"\\sources\\boot.sdi",
        L"\\EFI\\boot\\boot.sdi",
        L"\\EFI\\Microsoft\\Boot\\boot.sdi",
        L"\\SSTR\\boot.sdi",
        L"\\WEPE\\WEPE.SDI",
        L"\\boot.sdi"
    };
    for (const wchar_t* rel : relCandidates) {
        std::wstring full = JoinPath(mediaRoot, rel);
        if (FileExistsRegular(full)) {
            outSdiRelPath = rel;
            return full;
        }
    }

    // 3. Any *.sdi at mediaRoot
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(JoinPath(mediaRoot, L"*.sdi").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        std::wstring picked;
        std::wstring pickedName;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                picked     = JoinPath(mediaRoot, fd.cFileName);
                pickedName = fd.cFileName;
                break;
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
        if (!picked.empty()) {
            outSdiRelPath = L"\\" + pickedName;
            return picked;
        }
    }

    return L"";
}

bool ParseSingleBcdStore(const std::wstring& bcdFilePath,
                         const std::wstring& mediaRoot,
                         std::vector<CustomPeBootEntry>& outEntries,
                         const LogCallback& logCb) {
    RegHiveReader hive;
    if (!hive.load(bcdFilePath)) {
        return false;
    }

    std::wstring rootName;
    std::vector<uint32_t> rootSubs;
    std::map<std::wstring, BcdElementVal> rootVals;
    if (!hive.readNk(hive.rootOffset(), rootName, rootSubs, rootVals)) {
        return false;
    }

    std::map<std::wstring, BcdObjectNode> objects;
    for (uint32_t rs : rootSubs) {
        std::wstring sName;
        std::vector<uint32_t> objSubs;
        std::map<std::wstring, BcdElementVal> sVals;
        if (!hive.readNk(rs, sName, objSubs, sVals) || ToLower(sName) != L"objects") continue;

        for (uint32_t objOff : objSubs) {
            std::wstring guidStr;
            std::vector<uint32_t> oSubs;
            std::map<std::wstring, BcdElementVal> oVals;
            if (!hive.readNk(objOff, guidStr, oSubs, oVals)) continue;

            BcdObjectNode node{};
            node.guid = ToLower(Trim(guidStr));
            for (uint32_t subOff : oSubs) {
                std::wstring kName;
                std::vector<uint32_t> kSubs;
                std::map<std::wstring, BcdElementVal> kVals;
                if (!hive.readNk(subOff, kName, kSubs, kVals)) continue;

                std::wstring lowK = ToLower(kName);
                if (lowK == L"description") {
                    auto itT = kVals.find(L"type");
                    if (itT != kVals.end()) {
                        node.objType = static_cast<uint32_t>(itT->second.asUint64(0));
                    }
                } else if (lowK == L"elements") {
                    for (uint32_t elOff : kSubs) {
                        std::wstring elHex;
                        std::vector<uint32_t> eSubs;
                        std::map<std::wstring, BcdElementVal> eVals;
                        if (!hive.readNk(elOff, elHex, eSubs, eVals)) continue;
                        auto itE = eVals.find(L"element");
                        if (itE != eVals.end()) {
                            node.elements[ToLower(Trim(elHex))] = itE->second;
                        }
                    }
                }
            }
            if (!node.guid.empty()) {
                objects[node.guid] = std::move(node);
            }
        }
    }

    if (objects.empty()) return false;

    std::wstring defaultGuid;
    std::vector<std::wstring> displayOrder;
    std::map<std::wstring, std::wstring> sdiPathsByGuid;
    std::wstring anySdiRelPath;

    for (const auto& kv : objects) {
        const BcdObjectNode& n = kv.second;
        if (n.objType == 0x10100002u || n.guid == L"{9dea862c-5cdd-4e70-acc1-f32b344d4795}") {
            auto itDef = n.elements.find(L"23000003");
            if (itDef != n.elements.end()) defaultGuid = ToLower(itDef->second.asSz());
            auto itOrd = n.elements.find(L"24000001");
            if (itOrd != n.elements.end()) {
                for (const auto& g : itOrd->second.asMultiSz()) {
                    displayOrder.push_back(ToLower(g));
                }
            }
        } else if (n.objType == 0x30000000u) {
            auto itSdi = n.elements.find(L"32000004");
            if (itSdi != n.elements.end()) {
                std::wstring sp = itSdi->second.asSz();
                for (auto& ch : sp) {
                    if (ch == L'/') ch = L'\\';
                }
                if (!sp.empty()) {
                    if (sp.front() != L'\\') sp = L"\\" + sp;
                    sdiPathsByGuid[n.guid] = sp;
                    if (anySdiRelPath.empty()) anySdiRelPath = sp;
                }
            }
        }
    }

    const std::wstring kGlobalSettingsGuid     = L"{7ea2e1ac-2e61-4728-aaa3-896d9d0a9f0e}";
    const std::wstring kBootloaderSettingsGuid = L"{6efb52bf-1766-41db-a6b3-0ee5eff72bd7}";

    for (const auto& kv : objects) {
        const BcdObjectNode& n = kv.second;
        bool isOsLoader = (n.objType == 0x10200003u) || ((n.objType & 0xFFFF0000u) == 0x10200000u);
        if (!isOsLoader) continue;

        std::map<std::wstring, BcdElementVal> eff;
        std::set<std::wstring> visited;
        MergeInheritedBcdElements(kGlobalSettingsGuid, objects, eff, visited, 0);
        MergeInheritedBcdElements(kBootloaderSettingsGuid, objects, eff, visited, 0);
        MergeInheritedBcdElements(n.guid, objects, eff, visited, 0);

        std::wstring sdiGuid;
        std::wstring wimRel;
        auto itDev = eff.find(L"11000001");
        bool gotWim = (itDev != eff.end()) && ParseRamdiskDeviceBlob(itDev->second.raw, sdiGuid, wimRel);
        if (!gotWim) {
            auto itOsDev = eff.find(L"21000001");
            gotWim = (itOsDev != eff.end()) && ParseRamdiskDeviceBlob(itOsDev->second.raw, sdiGuid, wimRel);
        }
        if (!gotWim || wimRel.empty()) continue;

        if (ToLower(wimRel).rfind(L"[boot]", 0) == 0) {
            wimRel = wimRel.substr(6);
            if (!wimRel.empty() && wimRel.front() != L'\\') wimRel = L"\\" + wimRel;
        }

        std::wstring wimFull = JoinPath(mediaRoot, wimRel);
        if (!FileExistsRegular(wimFull)) {
            std::wstring parentRoot = ParentDirOf(mediaRoot);
            if (!parentRoot.empty() && FileExistsRegular(JoinPath(parentRoot, wimRel))) {
                wimFull = JoinPath(parentRoot, wimRel);
            } else {
                if (logCb) {
                    logCb(L" • BCD 항목 건너뜀 (미디어 내 WIM 파일 없음): " + wimRel);
                }
                continue;
            }
        }

        CustomPeBootEntry entry{};
        entry.bcdGuid     = n.guid;
        entry.wimRelPath  = wimRel;
        entry.wimFullPath = wimFull;

        if (!sdiGuid.empty() && sdiPathsByGuid.count(sdiGuid) > 0) {
            entry.sdiRelPath = sdiPathsByGuid[sdiGuid];
        } else if (!anySdiRelPath.empty()) {
            entry.sdiRelPath = anySdiRelPath;
        }

        if (!entry.sdiRelPath.empty()) {
            std::wstring candSdi = JoinPath(mediaRoot, entry.sdiRelPath);
            if (FileExistsRegular(candSdi)) {
                entry.sdiFullPath = candSdi;
            }
        }
        if (entry.sdiFullPath.empty()) {
            entry.sdiFullPath = FindFallbackSdiOnMedia(mediaRoot, entry.wimFullPath, entry.sdiRelPath);
        }

        auto itDesc = eff.find(L"12000004");
        if (itDesc != eff.end()) entry.description = itDesc->second.asSz();
        if (entry.description.empty()) {
            entry.description = StemOf(FileNameOf(entry.wimFullPath));
        }

        auto itPath = eff.find(L"12000002");
        if (itPath != eff.end()) entry.loaderPath = itPath->second.asSz();

        auto itSys = eff.find(L"22000002");
        entry.systemRoot = (itSys != eff.end() && !itSys->second.asSz().empty())
                           ? itSys->second.asSz()
                           : L"\\windows";

        auto itWinpe = eff.find(L"26000022");
        if (itWinpe != eff.end()) {
            entry.hasWinpeFlag = true;
            entry.winpeMode    = itWinpe->second.asBool(true);
        }

        auto itHal = eff.find(L"26000010");
        if (itHal != eff.end()) {
            entry.hasDetectHalFlag = true;
            entry.detectHal        = itHal->second.asBool(true);
        }

        auto itNx = eff.find(L"25000020");
        if (itNx != eff.end()) {
            switch (itNx->second.asUint64()) {
                case 0: entry.nxPolicy = L"OptIn"; break;
                case 1: entry.nxPolicy = L"OptOut"; break;
                case 2: entry.nxPolicy = L"AlwaysOff"; break;
                case 3: entry.nxPolicy = L"AlwaysOn"; break;
                default: break;
            }
        }

        std::vector<std::wstring> extraTokens;
        auto itNoInt = eff.find(L"16000048");
        if (itNoInt != eff.end() && itNoInt->second.asBool(false)) {
            entry.hasNoIntegrity = true;
            extraTokens.push_back(L"nointegritychecks yes");
        }

        auto itTestSig = eff.find(L"16000049");
        if (itTestSig != eff.end() && itTestSig->second.asBool(false)) {
            entry.hasTestSigning = true;
            extraTokens.push_back(L"testsigning yes");
        }

        auto itLoadOpt = eff.find(L"12000030");
        if (itLoadOpt != eff.end()) {
            std::wstring lo = itLoadOpt->second.asSz();
            if (!lo.empty()) {
                bool hasSpace = (lo.find(L' ') != std::wstring::npos);
                extraTokens.push_back(hasSpace ? (L"loadoptions \"" + lo + L"\"") : (L"loadoptions " + lo));
            }
        }

        auto itBmp = eff.find(L"250000c2");
        if (itBmp != eff.end()) {
            uint64_t bmp = itBmp->second.asUint64();
            if (bmp == 0) extraTokens.push_back(L"bootmenupolicy Legacy");
            else if (bmp == 1) extraTokens.push_back(L"bootmenupolicy Standard");
        }

        auto itEms = eff.find(L"260000b0");
        if (itEms != eff.end() && itEms->second.asBool(false)) {
            extraTokens.push_back(L"ems yes");
        }

        auto itPae = eff.find(L"25000021");
        if (itPae != eff.end()) {
            uint64_t pae = itPae->second.asUint64();
            if (pae == 1) extraTokens.push_back(L"pae ForceEnable");
            else if (pae == 2) extraTokens.push_back(L"pae ForceDisable");
        }

        auto itVga = eff.find(L"26000040");
        if (itVga != eff.end() && itVga->second.asBool(false)) {
            extraTokens.push_back(L"vga yes");
        }

        auto itHyp = eff.find(L"250000f0");
        if (itHyp != eff.end()) {
            uint64_t hv = itHyp->second.asUint64();
            if (hv == 0) extraTokens.push_back(L"hypervisorlaunchtype Off");
            else if (hv == 1) extraTokens.push_back(L"hypervisorlaunchtype Auto");
        }

        auto itBsp = eff.find(L"250000e0");
        if (itBsp != eff.end()) {
            uint64_t bsp = itBsp->second.asUint64();
            if (bsp == 1) extraTokens.push_back(L"bootstatuspolicy IgnoreAllFailures");
            else if (bsp == 2) extraTokens.push_back(L"bootstatuspolicy IgnoreShutdownFailures");
            else if (bsp == 3) extraTokens.push_back(L"bootstatuspolicy IgnoreBootFailures");
        }

        for (size_t i = 0; i < extraTokens.size(); ++i) {
            if (i > 0) entry.extraSetArgs += L"; ";
            entry.extraSetArgs += extraTokens[i];
        }

        entry.isDefault = (!defaultGuid.empty() && n.guid == defaultGuid);
        for (size_t idx = 0; idx < displayOrder.size(); ++idx) {
            if (displayOrder[idx] == n.guid) {
                entry.displayOrderRank = static_cast<int>(idx);
                break;
            }
        }

        entry.wimInfo = InspectWimFile(entry.wimFullPath);
        outEntries.push_back(std::move(entry));
    }

    return !outEntries.empty();
}

} // namespace

bool MountIsoReadOnly(const std::wstring& isoFullPath,
                      IsoMountSession& outSession,
                      const LogCallback& logCb) {
    outSession = IsoMountSession{};
    outSession.isoFullPath = isoFullPath;

    if (!FileExistsRegular(isoFullPath)) {
        if (logCb) logCb(L"오류: 선택한 ISO 파일을 찾을 수 없습니다: " + isoFullPath);
        return false;
    }

    if (logCb) logCb(L"커스텀 PE ISO 이미지 마운트 시도 중 (읽기 전용): " + isoFullPath);

    // 1. Primary path: dynamically loaded virtdisk.dll (fast, zero PowerShell startup overhead)
    HMODULE hVirtDisk = LoadLibraryW(L"virtdisk.dll");
    if (hVirtDisk) {
        using FnOpenVirtualDisk = DWORD(WINAPI*)(PVIRTUAL_STORAGE_TYPE, PCWSTR, VIRTUAL_DISK_ACCESS_MASK,
                                                 OPEN_VIRTUAL_DISK_FLAG, POPEN_VIRTUAL_DISK_PARAMETERS, PHANDLE);
        using FnAttachVirtualDisk = DWORD(WINAPI*)(HANDLE, PSECURITY_DESCRIPTOR, ATTACH_VIRTUAL_DISK_FLAG,
                                                   ULONG, PATTACH_VIRTUAL_DISK_PARAMETERS, LPOVERLAPPED);
        using FnDetachVirtualDisk = DWORD(WINAPI*)(HANDLE, DETACH_VIRTUAL_DISK_FLAG, ULONG);
        using FnGetVirtualDiskPhysicalPath = DWORD(WINAPI*)(HANDLE, PULONG, PWSTR);

        auto pOpen = reinterpret_cast<FnOpenVirtualDisk>(
            reinterpret_cast<void*>(GetProcAddress(hVirtDisk, "OpenVirtualDisk")));
        auto pAttach = reinterpret_cast<FnAttachVirtualDisk>(
            reinterpret_cast<void*>(GetProcAddress(hVirtDisk, "AttachVirtualDisk")));
        auto pDetach = reinterpret_cast<FnDetachVirtualDisk>(
            reinterpret_cast<void*>(GetProcAddress(hVirtDisk, "DetachVirtualDisk")));
        auto pGetPhys = reinterpret_cast<FnGetVirtualDiskPhysicalPath>(
            reinterpret_cast<void*>(GetProcAddress(hVirtDisk, "GetVirtualDiskPhysicalPath")));

        if (pOpen && pAttach && pGetPhys) {
            VIRTUAL_STORAGE_TYPE vst{};
            vst.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_ISO;
            // VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT {EC984AEC-A0F9-47e9-901F-71415A66345B}
            vst.VendorId = { 0xEC984AECu, 0xA0F9u, 0x47E9u, { 0x90, 0x1F, 0x71, 0x41, 0x5A, 0x66, 0x34, 0x5B } };

            HANDLE hVhd = INVALID_HANDLE_VALUE;
            DWORD st = pOpen(&vst, isoFullPath.c_str(), VIRTUAL_DISK_ACCESS_READ,
                             OPEN_VIRTUAL_DISK_FLAG_NONE, nullptr, &hVhd);
            if (st == ERROR_SUCCESS && hVhd != INVALID_HANDLE_VALUE) {
                wchar_t physBuf[MAX_PATH]{};
                ULONG physCch = _countof(physBuf);
                bool alreadyAttached = (pGetPhys(hVhd, &physCch, physBuf) == ERROR_SUCCESS && physBuf[0] != L'\0');
                DWORD drivesBefore = GetLogicalDrives();

                if (!alreadyAttached) {
                    st = pAttach(hVhd, nullptr, ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY, 0, nullptr, nullptr);
                }

                if (alreadyAttached || st == ERROR_SUCCESS) {
                    std::wstring foundRoot;
                    for (int attempt = 0; attempt < 40; ++attempt) {
                        physCch = _countof(physBuf);
                        physBuf[0] = L'\0';
                        pGetPhys(hVhd, &physCch, physBuf);
                        DWORD newMask = alreadyAttached ? 0u : (GetLogicalDrives() & ~drivesBefore);
                        foundRoot = ResolvePhysicalCdromToDriveRoot(physBuf, newMask);
                        if (!foundRoot.empty()) {
                            // Wait briefly until UDF/ISO9660 filesystem is ready
                            if (GetVolumeInformationW(foundRoot.c_str(), nullptr, 0, nullptr,
                                                      nullptr, nullptr, nullptr, 0)) {
                                break;
                            }
                        }
                        Sleep(100);
                    }

                    if (!foundRoot.empty()) {
                        outSession.mounted     = true;
                        outSession.mountedByUs = !alreadyAttached;
                        outSession.driveRoot   = foundRoot;
                        if (alreadyAttached) {
                            CloseHandle(hVhd);
                            outSession.vhdHandle = INVALID_HANDLE_VALUE;
                        } else {
                            outSession.vhdHandle = hVhd; // keep open until UnmountIsoSession
                        }
                        FreeLibrary(hVirtDisk);
                        if (logCb) {
                            logCb(L"ISO 마운트 확인 완료 -> 드라이브 " + outSession.driveRoot +
                                  (alreadyAttached ? L" (기존 마운트 사용)" : L" (임시 마운트됨)"));
                        }
                        return true;
                    }

                    if (!alreadyAttached && pDetach) {
                        pDetach(hVhd, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
                    }
                }
                CloseHandle(hVhd);
            }
        }
        FreeLibrary(hVirtDisk);
    }

    // 2. Fallback path: PowerShell Mount-DiskImage
    if (logCb) logCb(L"virtdisk 직접 마운트 대신 PowerShell Mount-DiskImage로 재시도합니다...");
    std::wstring escIso = EscapePsSingleQuoted(isoFullPath);
    std::wstring psCmd =
        L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \""
        L"$p = '" + escIso + L"'; "
        L"$pre = (Get-DiskImage -ImagePath $p).Attached; "
        L"if (-not $pre) { Mount-DiskImage -ImagePath $p -StorageType ISO -Access ReadOnly | Out-Null }; "
        L"$dl = (Get-DiskImage -ImagePath $p | Get-Volume).DriveLetter; "
        L"Write-Output ('OTPE_ISO=' + [int][bool]$pre + ':' + $dl)\"";

    ExecOutput psOut = RunHiddenProcess(L"WindowsPowerShell\\v1.0\\powershell.exe", psCmd, nullptr);
    size_t tagPos = psOut.text.find(L"OTPE_ISO=");
    if (tagPos != std::wstring::npos && tagPos + 12 <= psOut.text.size()) {
        wchar_t preCh = psOut.text[tagPos + 9];
        wchar_t drvCh = psOut.text[tagPos + 11];
        if (std::iswalpha(drvCh)) {
            wchar_t root[4] = { static_cast<wchar_t>(std::towupper(drvCh)), L':', L'\\', L'\0' };
            outSession.mounted        = true;
            outSession.mountedByUs    = (preCh == L'0');
            outSession.usedPowerShell = true;
            outSession.driveRoot      = root;
            if (logCb) logCb(L"PowerShell ISO 마운트 완료 -> 드라이브 " + outSession.driveRoot);
            return true;
        }
    }

    if (logCb) logCb(L"오류: ISO 이미지를 마운트하지 못했습니다.");
    return false;
}

void UnmountIsoSession(IsoMountSession& session, const LogCallback& logCb) {
    if (!session.mounted) return;

    if (session.vhdHandle != INVALID_HANDLE_VALUE) {
        if (session.mountedByUs) {
            HMODULE hVirtDisk = LoadLibraryW(L"virtdisk.dll");
            if (hVirtDisk) {
                using FnDetachVirtualDisk = DWORD(WINAPI*)(HANDLE, DETACH_VIRTUAL_DISK_FLAG, ULONG);
                auto pDetach = reinterpret_cast<FnDetachVirtualDisk>(
                    reinterpret_cast<void*>(GetProcAddress(hVirtDisk, "DetachVirtualDisk")));
                if (pDetach) {
                    pDetach(session.vhdHandle, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
                }
                FreeLibrary(hVirtDisk);
            }
        }
        CloseHandle(session.vhdHandle);
        session.vhdHandle = INVALID_HANDLE_VALUE;
        if (session.mountedByUs && logCb) {
            logCb(L"임시 마운트했던 ISO 이미지를 자동 언마운트했습니다 (" + session.driveRoot + L")");
        }
    } else if (session.mountedByUs && session.usedPowerShell && !session.isoFullPath.empty()) {
        std::wstring escIso = EscapePsSingleQuoted(session.isoFullPath);
        std::wstring psCmd =
            L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \""
            L"Dismount-DiskImage -ImagePath '" + escIso + L"' -ErrorAction SilentlyContinue | Out-Null\"";
        RunHiddenProcess(L"WindowsPowerShell\\v1.0\\powershell.exe", psCmd, nullptr);
        if (logCb) {
            logCb(L"임시 마운트했던 ISO 이미지를 자동 언마운트했습니다 (" + session.driveRoot + L")");
        }
    }

    session.mounted     = false;
    session.mountedByUs = false;
}

CustomPeDiscoveryResult AnalyzeCustomPeMedia(const std::wstring& selectedPath,
                                             bool preferUefi,
                                             const LogCallback& logCb) {
    CustomPeDiscoveryResult res{};
    std::wstring cleanPath = Trim(selectedPath);
    if (cleanPath.empty()) {
        res.errorMsg = L"선택된 경로가 비어 있습니다.";
        return res;
    }

    std::wstring explicitBcdPath;
    std::wstring preferredWimPath;
    std::wstring mediaRoot;

    if (DirExists(cleanPath)) {
        mediaRoot = cleanPath;
        // If user selected a nested folder like ...\EFI\Microsoft\Boot or ...\sources, step up to PE media root
        for (int step = 0; step < 3; ++step) {
            std::wstring folderLow = ToLower(FileNameOf(mediaRoot));
            std::wstring up = ParentDirOf(mediaRoot);
            if (up.empty()) break;
            if (folderLow == L"boot" || folderLow == L"microsoft" || folderLow == L"efi" ||
                folderLow == L"pcat" || folderLow == L"sources") {
                mediaRoot = up;
            } else {
                break;
            }
        }
    } else if (FileExistsRegular(cleanPath)) {
        std::wstring fnLow = ToLower(FileNameOf(cleanPath));
        if (EndsWithLower(fnLow, L".wim") || EndsWithLower(fnLow, L".esd")) {
            preferredWimPath = cleanPath;
            mediaRoot = ParentDirOf(cleanPath);
            std::wstring parentNameLow = ToLower(FileNameOf(mediaRoot));
            if (parentNameLow == L"sources" || parentNameLow == L"boot" ||
                parentNameLow == L"sstr" || parentNameLow == L"wepe" ||
                parentNameLow == L"wim" || parentNameLow == L"pe") {
                std::wstring up = ParentDirOf(mediaRoot);
                if (!up.empty()) mediaRoot = up;
            }
        } else {
            // Treat as BCD store file
            explicitBcdPath = cleanPath;
            mediaRoot = ParentDirOf(cleanPath);
            for (int step = 0; step < 4; ++step) {
                std::wstring folderLow = ToLower(FileNameOf(mediaRoot));
                std::wstring up = ParentDirOf(mediaRoot);
                if (up.empty()) break;
                if (folderLow == L"boot" || folderLow == L"microsoft" || folderLow == L"efi" ||
                    folderLow == L"pcat" || folderLow == L"dvd") {
                    mediaRoot = up;
                } else {
                    break;
                }
            }
        }
    } else {
        res.errorMsg = L"선택한 파일 또는 폴더를 찾을 수 없습니다:\n" + cleanPath;
        return res;
    }

    res.mediaRoot = mediaRoot;
    if (logCb) logCb(L"--- 커스텀 PE 미디어 구조 및 BCD 분석 시작 (루트: " + mediaRoot + L") ---");

    // 1. Build ordered list of candidate BCD hives
    std::vector<std::wstring> bcdCandidates;
    auto addCandidate = [&](const std::wstring& p) {
        if (!FileExistsRegular(p)) return;
        for (const auto& existing : bcdCandidates) {
            if (_wcsicmp(existing.c_str(), p.c_str()) == 0) return;
        }
        bcdCandidates.push_back(p);
    };

    if (!explicitBcdPath.empty()) addCandidate(explicitBcdPath);
    if (preferUefi) {
        addCandidate(JoinPath(mediaRoot, L"EFI\\Microsoft\\Boot\\BCD"));
        addCandidate(JoinPath(mediaRoot, L"EFI\\Boot\\BCD"));
        addCandidate(JoinPath(mediaRoot, L"SSTR\\BCD"));
        addCandidate(JoinPath(mediaRoot, L"boot\\bcd"));
    } else {
        addCandidate(JoinPath(mediaRoot, L"boot\\bcd"));
        addCandidate(JoinPath(mediaRoot, L"SSTR\\BCD"));
        addCandidate(JoinPath(mediaRoot, L"EFI\\Microsoft\\Boot\\BCD"));
        addCandidate(JoinPath(mediaRoot, L"EFI\\Boot\\BCD"));
    }
    addCandidate(JoinPath(mediaRoot, L"BCD"));

    for (const auto& bcdPath : bcdCandidates) {
        std::vector<CustomPeBootEntry> parsed;
        if (ParseSingleBcdStore(bcdPath, mediaRoot, parsed, logCb) && !parsed.empty()) {
            res.usedBcdPath = bcdPath;
            res.entries     = std::move(parsed);
            if (logCb) {
                logCb(L"BCD 바이너리 하이브 분석 성공: " + bcdPath +
                      L" (부팅 가능 WIM 항목 " + std::to_wstring(res.entries.size()) + L"개 발견)");
            }
            break;
        }
    }

    // 2. Fallback: If media has NO BCD store (e.g. extracted WIM + SDI folder), scan for .wim / .sdi directly
    if (res.entries.empty()) {
        std::vector<std::wstring> foundWims;
        auto addWimsInDir = [&](const std::wstring& subRel) {
            std::wstring dir = subRel.empty() ? mediaRoot : JoinPath(mediaRoot, subRel);
            if (!DirExists(dir)) return;
            WIN32_FIND_DATAW fd{};
            HANDLE hFind = FindFirstFileW(JoinPath(dir, L"*.wim").c_str(), &fd);
            if (hFind == INVALID_HANDLE_VALUE) return;
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    std::wstring full = JoinPath(dir, fd.cFileName);
                    bool dup = false;
                    for (const auto& ex : foundWims) {
                        if (_wcsicmp(ex.c_str(), full.c_str()) == 0) { dup = true; break; }
                    }
                    if (!dup) foundWims.push_back(full);
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        };

        if (!preferredWimPath.empty() && FileExistsRegular(preferredWimPath)) {
            foundWims.push_back(preferredWimPath);
        }
        addWimsInDir(L"");
        addWimsInDir(L"sources");
        addWimsInDir(L"boot");
        addWimsInDir(L"SSTR");
        addWimsInDir(L"WEPE");
        addWimsInDir(L"WIM");
        addWimsInDir(L"PE");

        // Filter out huge install.wim if other PE wims exist
        if (foundWims.size() > 1) {
            std::vector<std::wstring> nonInstall;
            for (const auto& w : foundWims) {
                if (_wcsicmp(FileNameOf(w).c_str(), L"install.wim") != 0) {
                    nonInstall.push_back(w);
                }
            }
            if (!nonInstall.empty()) foundWims = std::move(nonInstall);
        }

        for (size_t i = 0; i < foundWims.size(); ++i) {
            CustomPeBootEntry e{};
            e.wimFullPath = foundWims[i];
            if (ToLower(e.wimFullPath).rfind(ToLower(mediaRoot), 0) == 0) {
                e.wimRelPath = e.wimFullPath.substr(mediaRoot.size());
                if (!e.wimRelPath.empty() && e.wimRelPath.front() != L'\\') e.wimRelPath = L"\\" + e.wimRelPath;
            } else {
                e.wimRelPath = L"\\" + FileNameOf(e.wimFullPath);
            }
            e.sdiFullPath      = FindFallbackSdiOnMedia(mediaRoot, e.wimFullPath, e.sdiRelPath);
            e.wimInfo          = InspectWimFile(e.wimFullPath);
            e.description      = !e.wimInfo.imageName.empty() ? e.wimInfo.imageName : StemOf(FileNameOf(e.wimFullPath));
            e.loaderPath       = preferUefi ? L"\\windows\\system32\\winload.efi" : L"\\windows\\system32\\winload.exe";
            e.systemRoot       = !e.wimInfo.systemRoot.empty() ? e.wimInfo.systemRoot : L"\\windows";
            e.hasWinpeFlag     = true;
            e.winpeMode        = true;
            e.hasDetectHalFlag = true;
            e.detectHal        = true;
            e.displayOrderRank = static_cast<int>(i);
            res.entries.push_back(std::move(e));
        }

        if (!res.entries.empty()) {
            res.synthesizedWithoutBcd = true;
            if (logCb) {
                logCb(L"참고: 미디어 내 BCD 파일이 없어 폴더 스캔으로 WIM 항목 " +
                      std::to_wstring(res.entries.size()) + L"개를 구성했습니다.");
            }
        }
    }

    if (res.entries.empty()) {
        res.errorMsg =
            L"선택한 경로에서 부팅 가능한 커스텀 PE(.wim) 및 BCD 구성을 찾지 못했습니다.\n\n"
            L"확인할 경로: " + mediaRoot + L"\n"
            L"올바른 커스텀 PE .iso 파일, 압축 해제된 PE 루트 폴더, 또는 EFI\\Microsoft\\Boot\\BCD 파일을 선택해 주세요.";
        return res;
    }

    // 3. Sort entries: preferredWimPath match -> architecture match (x64 first on UEFI) -> default -> displayOrder
    std::stable_sort(res.entries.begin(), res.entries.end(),
                     [&](const CustomPeBootEntry& a, const CustomPeBootEntry& b) {
                         if (!preferredWimPath.empty()) {
                             bool aMatch = (_wcsicmp(a.wimFullPath.c_str(), preferredWimPath.c_str()) == 0);
                             bool bMatch = (_wcsicmp(b.wimFullPath.c_str(), preferredWimPath.c_str()) == 0);
                             if (aMatch != bMatch) return aMatch;
                         }
                         if (preferUefi) {
                             bool aBadArch = (a.wimInfo.arch == 0);
                             bool bBadArch = (b.wimInfo.arch == 0);
                             if (aBadArch != bBadArch) return !aBadArch;
                         }
                         if (a.isDefault != b.isDefault) return a.isDefault;
                         return a.displayOrderRank < b.displayOrderRank;
                     });

    // 4. Detect external tool folders / markers at mediaRoot
    const wchar_t* toolMarkers[] = {
        L"Programs", L"PETools", L"SSTR", L"WEPE", L"DLC1", L"PESoft",
        L"Y.ini", L"CdUsb.Y", L"Win10XPE.ini", L"HBCD_PE.ini"
    };
    for (const wchar_t* mk : toolMarkers) {
        std::wstring fullMk = JoinPath(mediaRoot, mk);
        if (GetFileAttributesW(fullMk.c_str()) != INVALID_FILE_ATTRIBUTES) {
            res.externalToolMarkers.push_back(mk);
        }
    }

    res.ok = true;
    return res;
}

bool ExportNetworkConfigForPe(const std::wstring& appDir,
                              const std::wstring& appBcdRelDir,
                              bool exportDrivers,
                              const LogCallback& logCb,
                              std::wstring& outSummaryReport) {
    DetectedNetworkSummary net = DetectActiveWindowsNetwork();
    std::wstring netDir  = JoinPath(appDir, L"NetConfig");
    std::wstring drvDir  = JoinPath(netDir, L"Drivers");
    std::wstring wifiDir = JoinPath(netDir, L"WiFi");

    EnsureDirectoryRecursive(netDir);
    DeleteDirectoryContentsRecursive(wifiDir);
    EnsureDirectoryRecursive(wifiDir);

    if (logCb) logCb(L"--- 현재 Windows 네트워크 설정 및 드라이버 추출 시작 ---");

    // 1. Write netconfig.ini (UTF-16LE)
    std::wstring cfgIni = JoinPath(netDir, L"netconfig.ini");
    DeleteFileW(cfgIni.c_str());
    EnsureUnicodeIni(cfgIni);

    WritePrivateProfileStringW(L"General", L"AdapterCount",
                               std::to_wstring(net.adapters.size()).c_str(), cfgIni.c_str());
    WritePrivateProfileStringW(L"General", L"ExportDrivers",
                               exportDrivers ? L"1" : L"0", cfgIni.c_str());

    outSummaryReport.clear();
    outSummaryReport += L"[감지 및 추출된 네트워크 어댑터 (" + std::to_wstring(net.adapters.size()) + L"개)]\n";

    for (size_t i = 0; i < net.adapters.size(); ++i) {
        const auto& a = net.adapters[i];
        std::wstring sec = L"Adapter" + std::to_wstring(i);
        WritePrivateProfileStringW(sec.c_str(), L"IsWifi", a.isWifi ? L"1" : L"0", cfgIni.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"HardwareDesc", a.hardwareDesc.c_str(), cfgIni.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"MacAddress", a.macDash.c_str(), cfgIni.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"InfName", a.infName.c_str(), cfgIni.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"StaticIP", a.staticIp ? L"1" : L"0", cfgIni.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"IPv4", a.ipv4.c_str(), cfgIni.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"SubnetMask", a.subnetMask.c_str(), cfgIni.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"Gateway", a.gateway.c_str(), cfgIni.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"StaticDNS", a.staticDns ? L"1" : L"0", cfgIni.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"DNS1", a.dns1.c_str(), cfgIni.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"DNS2", a.dns2.c_str(), cfgIni.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"WifiProfile", a.wifiProfile.c_str(), cfgIni.c_str());

        outSummaryReport += L" • [" + std::wstring(a.isWifi ? L"Wi-Fi" : L"유선") + L"] " + a.hardwareDesc + L"\n";
        outSummaryReport += L"   - MAC 주소: " + a.macDash + L"  |  드라이버: " +
                            (a.infName.empty() ? L"Windows 기본 내장" : a.infName) + L"\n";
        outSummaryReport += L"   - IP 방식 : " + std::wstring(a.staticIp ? L"고정 IP (" : L"자동 DHCP (현재 ") +
                            a.ipv4 + L" / " + a.subnetMask +
                            (a.gateway.empty() ? L")" : (L", GW: " + a.gateway + L")")) + L"\n";
        outSummaryReport += L"   - DNS 방식: " + std::wstring(a.staticDns ? L"수동 DNS (" : L"자동 DNS (현재 ") +
                            (a.dns1.empty() ? L"없음" : a.dns1) +
                            (a.dns2.empty() ? L")" : (L", " + a.dns2 + L")")) + L"\n";

        // 2. Export Wi-Fi profile XML (with plaintext key) if Wi-Fi is connected
        if (a.isWifi && !a.wifiProfile.empty()) {
            std::wstring args = L"wlan export profile name=\"" + a.wifiProfile +
                                L"\" key=clear folder=\"" + wifiDir + L"\"";
            ExecOutput wr = RunHiddenProcess(L"netsh.exe", args, logCb);
            if (wr.exitCode == 0) {
                outSummaryReport += L"   - Wi-Fi 프로필 추출 완료: " + a.wifiProfile + L"\n";
            } else {
                outSummaryReport += L"   - Wi-Fi 프로필 추출 실패: " + a.wifiProfile + L"\n";
            }
        }
    }

    if (net.adapters.empty()) {
        outSummaryReport += L" • 현재 활성화된 인터넷/LAN 연결이 감지되지 않았습니다 (기본 DHCP 초기화만 수행됨).\n";
    }

    // 3. Export physical NIC drivers (.inf/.sys/.cat) via pnputil
    if (exportDrivers && !net.physicalOemInfs.empty()) {
        DeleteDirectoryContentsRecursive(drvDir);
        EnsureDirectoryRecursive(drvDir);
        outSummaryReport += L"\n[추출된 물리 랜카드 드라이버 패키지]\n";
        for (const auto& inf : net.physicalOemInfs) {
            ExecOutput dr = RunHiddenProcess(L"pnputil.exe",
                                             L"/export-driver " + inf + L" \"" + drvDir + L"\"",
                                             logCb);
            if (dr.exitCode == 0) {
                outSummaryReport += L" • " + inf + L" -> NetConfig\\Drivers 추출 완료\n";
            } else {
                outSummaryReport += L" • " + inf + L" 추출 실패 (종료코드 " + std::to_wstring(dr.exitCode) + L")\n";
            }
        }
    } else if (!exportDrivers) {
        outSummaryReport += L"\n[드라이버 추출] 옵션 꺼짐 (건너뜀)\n";
    } else {
        outSummaryReport += L"\n[드라이버 추출] 서드파티 OEM 랜카드 드라이버 없음 (Windows 기본 인박스 드라이버 사용 중)\n";
    }

    // 4. Generate ApplyPENetwork.cmd inside NetConfig (manual / fallback launcher)
    std::string fallbackCmd =
        "@echo off\r\n"
        "setlocal\r\n"
        "set \"BASE_DIR=%~dp0..\"\r\n"
        "if exist \"%BASE_DIR%\\OneTimePE.exe\" (\r\n"
        "    \"%BASE_DIR%\\OneTimePE.exe\" --pe-apply-net\r\n"
        "    exit /b %ERRORLEVEL%\r\n"
        ")\r\n"
        "for /r \"%~dp0Drivers\" %%F in (*.inf) do drvload.exe \"%%~fF\" >nul 2>&1\r\n"
        "wpeutil.exe InitializeNetwork >nul 2>&1\r\n"
        "for %%X in (\"%~dp0WiFi\\*.xml\") do netsh.exe wlan add profile filename=\"%%~fX\" user=all >nul 2>&1\r\n"
        "endlocal\r\n";
    WriteAsciiFile(JoinPath(netDir, L"ApplyPENetwork.cmd"), fallbackCmd);

    // 5. Snapshot Windows drive map + write UTF-16LE Unicode bridge & copy self to %SystemRoot%\Temp
    ExportDriveMapForPe(appBcdRelDir, true, logCb);

    outSummaryReport += L"\n저장 폴더: " + netDir;
    if (logCb) logCb(L"네트워크 설정 및 드라이버 추출이 완료되었습니다.");
    return true;
}

namespace {

#ifndef IOCTL_MOUNTMGR_SET_AUTO_MOUNT
#define IOCTL_MOUNTMGR_SET_AUTO_MOUNT CTL_CODE(0x0000006D, 15, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif

#ifndef IOCTL_DISK_GET_DISK_ATTRIBUTES
#define IOCTL_DISK_GET_DISK_ATTRIBUTES CTL_CODE(IOCTL_DISK_BASE, 0x003c, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef IOCTL_DISK_SET_DISK_ATTRIBUTES
#define IOCTL_DISK_SET_DISK_ATTRIBUTES CTL_CODE(IOCTL_DISK_BASE, 0x003d, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif

#ifndef IOCTL_VOLUME_SET_GPT_ATTRIBUTES
#define IOCTL_VOLUME_SET_GPT_ATTRIBUTES CTL_CODE(IOCTL_VOLUME_BASE, 13, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef IOCTL_VOLUME_GET_GPT_ATTRIBUTES
#define IOCTL_VOLUME_GET_GPT_ATTRIBUTES CTL_CODE(IOCTL_VOLUME_BASE, 14, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef GPT_BASIC_DATA_ATTRIBUTE_READ_ONLY
#define GPT_BASIC_DATA_ATTRIBUTE_READ_ONLY 0x1000000000000000ULL
#endif
#ifndef GPT_BASIC_DATA_ATTRIBUTE_HIDDEN
#define GPT_BASIC_DATA_ATTRIBUTE_HIDDEN 0x4000000000000000ULL
#endif
#ifndef GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER
#define GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER 0x8000000000000000ULL
#endif

struct OtpeGetDiskAttrs {
    DWORD     version;
    DWORD     reserved1;
    ULONGLONG attributes;
};
static_assert(sizeof(OtpeGetDiskAttrs) == 16, "OtpeGetDiskAttrs ABI size mismatch");

struct OtpeSetDiskAttrs {
    DWORD     version;
    BOOLEAN   persist;
    BYTE      reserved1[3];
    ULONGLONG attributes;
    ULONGLONG attributesMask;
    DWORD     reserved2[4];
};
static_assert(sizeof(OtpeSetDiskAttrs) == 40, "OtpeSetDiskAttrs ABI size mismatch");

struct OtpeVolGetGptAttrs {
    ULONGLONG gptAttributes;
};
static_assert(sizeof(OtpeVolGetGptAttrs) == 8, "OtpeVolGetGptAttrs ABI size mismatch");

struct OtpeVolSetGptAttrs {
    ULONGLONG gptAttributes;
    BOOLEAN   revertOnClose;
    BOOLEAN   applyToAllConnectedVolumes;
    USHORT    reserved1;
    ULONG     reserved2;
};
static_assert(sizeof(OtpeVolSetGptAttrs) == 16, "OtpeVolSetGptAttrs ABI size mismatch");

[[maybe_unused]] constexpr GUID kGptTypeBasicData = { 0xebd0a0a2, 0xb9e5, 0x4433, { 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 } };
[[maybe_unused]] constexpr GUID kGptTypeEsp       = { 0xc12a7328, 0xf81f, 0x11d2, { 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b } };
[[maybe_unused]] constexpr GUID kGptTypeMsr       = { 0xe3c9e316, 0x0b5c, 0x4db8, { 0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae } };
[[maybe_unused]] constexpr GUID kGptTypeWinRe     = { 0xde94bba4, 0x06d1, 0x4d40, { 0xa1, 0x6a, 0xbf, 0xd5, 0x01, 0x79, 0xd6, 0xac } };

bool GuidEq(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

STORAGE_BUS_TYPE QueryDeviceBusType(const std::wstring& devPath) {
    HANDLE h = CreateFileW(devPath.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return BusTypeUnknown;

    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType  = PropertyStandardQuery;
    BYTE buf[1024]{};
    DWORD ret = 0;
    STORAGE_BUS_TYPE bt = BusTypeUnknown;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &ret, nullptr) &&
        ret >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf);
        bt = desc->BusType;
    }
    CloseHandle(h);
    return bt;
}

bool IsInternalFixedBus(STORAGE_BUS_TYPE bt) {
    if (bt == BusTypeUsb || bt == BusTypeSd || bt == BusTypeMmc ||
        bt == BusTypeVirtual || bt == BusTypeFileBackedVirtual) {
        return false;
    }
    return true;
}

bool QueryVolumePrimaryExtent(const std::wstring& volNoSlash,
                              DWORD& outDiskNum,
                              LONGLONG& outStartOffset,
                              LONGLONG& outExtentLen) {
    outDiskNum     = DWORD(-1);
    outStartOffset = -1;
    outExtentLen   = 0;
    HANDLE h = CreateFileW(volNoSlash.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    BYTE extBuf[512]{};
    DWORD ret = 0;
    bool ok = false;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                        extBuf, sizeof(extBuf), &ret, nullptr) &&
        ret >= sizeof(VOLUME_DISK_EXTENTS)) {
        const auto* vde = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(extBuf);
        if (vde->NumberOfDiskExtents > 0) {
            outDiskNum     = vde->Extents[0].DiskNumber;
            outStartOffset = vde->Extents[0].StartingOffset.QuadPart;
            outExtentLen   = vde->Extents[0].ExtentLength.QuadPart;
            ok = true;
        }
    }
    CloseHandle(h);
    return ok;
}

// Losslessly prepend a pure-ASCII command line to an INI/script file without re-encoding existing bytes
// (preserves CP949 Korean / GBK / CP1251 / UTF-8-no-BOM / UTF-8-BOM / UTF-16LE bit-for-bit!)
bool PrependLinePreservingRawEncoding(const std::wstring& filePath,
                                      const std::string& asciiLineWithCrlf,
                                      const std::string& needleLowerAscii,
                                      bool& outAlreadyPresent) {
    outAlreadyPresent = false;
    DWORD attr = GetFileAttributesW(filePath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    if (attr & FILE_ATTRIBUTE_READONLY) {
        SetFileAttributesW(filePath.c_str(), attr & ~FILE_ATTRIBUTE_READONLY);
    }

    HANDLE hIn = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hIn == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(hIn, nullptr);
    if (sz == INVALID_FILE_SIZE) {
        CloseHandle(hIn);
        return false;
    }
    std::vector<BYTE> raw(sz);
    DWORD rd = 0;
    if (sz > 0) ReadFile(hIn, raw.data(), sz, &rd, nullptr);
    CloseHandle(hIn);
    raw.resize(rd);

    bool isUtf16Le = (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE);
    bool isUtf8Bom = (!isUtf16Le && raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF);

    if (isUtf16Le) {
        std::wstring text(reinterpret_cast<const wchar_t*>(raw.data() + 2), (raw.size() - 2) / 2);
        std::wstring wNeedle(needleLowerAscii.begin(), needleLowerAscii.end());
        if (ToLower(text).find(wNeedle) != std::wstring::npos) {
            outAlreadyPresent = true;
            return true;
        }
        std::wstring wLine(asciiLineWithCrlf.begin(), asciiLineWithCrlf.end());
        HANDLE hOut = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hOut == INVALID_HANDLE_VALUE) return false;
        DWORD wr = 0;
        const BYTE bom[2] = { 0xFF, 0xFE };
        WriteFile(hOut, bom, 2, &wr, nullptr);
        WriteFile(hOut, wLine.data(), static_cast<DWORD>(wLine.size() * sizeof(wchar_t)), &wr, nullptr);
        if (raw.size() > 2) {
            WriteFile(hOut, raw.data() + 2, static_cast<DWORD>(raw.size() - 2), &wr, nullptr);
        }
        CloseHandle(hOut);
        return true;
    }

    std::string lowRaw(raw.size(), '\0');
    for (size_t i = 0; i < raw.size(); ++i) {
        unsigned char ch = raw[i];
        lowRaw[i] = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + 32) : static_cast<char>(ch);
    }
    if (lowRaw.find(needleLowerAscii) != std::string::npos) {
        outAlreadyPresent = true;
        return true;
    }

    HANDLE hOut = CreateFileW(filePath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    size_t bodyOffset = 0;
    if (isUtf8Bom) {
        const BYTE bom[3] = { 0xEF, 0xBB, 0xBF };
        WriteFile(hOut, bom, 3, &wr, nullptr);
        bodyOffset = 3;
    }
    WriteFile(hOut, asciiLineWithCrlf.data(), static_cast<DWORD>(asciiLineWithCrlf.size()), &wr, nullptr);
    if (raw.size() > bodyOffset) {
        WriteFile(hOut, raw.data() + bodyOffset, static_cast<DWORD>(raw.size() - bodyOffset), &wr, nullptr);
    }
    CloseHandle(hOut);
    return true;
}

} // namespace

bool ExportDriveMapForPe(const std::wstring& appBcdRelDir,
                         bool autoApplyNetwork,
                         const LogCallback& logCb,
                         bool remoteSupportEnable,
                         const std::wstring& remoteSupportCode,
                         const std::wstring& remoteSupportExe) {
    wchar_t winDir[MAX_PATH]{};
    if (GetWindowsDirectoryW(winDir, MAX_PATH) == 0) return false;

    wchar_t osLetter = (winDir[0] != L'\0' && winDir[1] == L':')
                       ? static_cast<wchar_t>(std::towupper(winDir[0]))
                       : L'C';

    std::wstring tempDir = JoinPath(winDir, L"Temp");
    EnsureDirectoryRecursive(tempDir);

    // 1. Snapshot all internal fixed drives (OS drive C: first, then D:, E:, ...)
    struct DriveRecord {
        wchar_t      letter      = 0;
        bool         isOs        = false;
        std::wstring volumeGuid;
        LONGLONG     startOffset = -1;
        LONGLONG     extentLen   = 0;
        DWORD        serial      = 0;
        std::wstring fsName;
    };
    std::vector<DriveRecord> records;

    DWORD mask = GetLogicalDrives();
    for (int pass = 0; pass < 2; ++pass) {
        for (int bit = 0; bit < 26; ++bit) {
            if ((mask & (1u << bit)) == 0) continue;
            wchar_t ch = static_cast<wchar_t>(L'A' + bit);
            if (ch == L'X') continue; // Reserved for WinPE RAMDisk
            bool isOs = (ch == osLetter);
            if ((pass == 0 && !isOs) || (pass == 1 && isOs)) continue;

            wchar_t rootSlash[4] = { ch, L':', L'\\', L'\0' };
            std::wstring devColon = std::wstring(L"\\\\.\\") + ch + L":";
            if (GetDriveTypeW(rootSlash) != DRIVE_FIXED) continue;
            STORAGE_BUS_TYPE bt = QueryDeviceBusType(devColon);
            if (!IsInternalFixedBus(bt)) continue;

            wchar_t volGuidBuf[128]{};
            if (!GetVolumeNameForVolumeMountPointW(rootSlash, volGuidBuf, _countof(volGuidBuf))) {
                continue;
            }

            DriveRecord rec;
            rec.letter     = ch;
            rec.isOs       = isOs;
            rec.volumeGuid = ToLower(volGuidBuf);

            DWORD diskNum = DWORD(-1);
            QueryVolumePrimaryExtent(devColon, diskNum, rec.startOffset, rec.extentLen);

            wchar_t fsBuf[64]{};
            DWORD serial = 0;
            if (GetVolumeInformationW(rootSlash, nullptr, 0, &serial, nullptr, nullptr, fsBuf, _countof(fsBuf))) {
                rec.serial = serial;
                rec.fsName = fsBuf;
            }
            records.push_back(std::move(rec));
        }
    }

    // Write pure-ASCII OneTimePE_DriveMap.ini so WinPE can parse it directly via \\?\Volume{GUID}\ before letters exist
    std::string mapIni = "[DriveMap]\r\n";
    mapIni += "Count=" + std::to_string(records.size()) + "\r\n";
    mapIni += std::string("OsLetter=") + static_cast<char>(osLetter) + "\r\n\r\n";

    for (size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        char serialHex[16]{};
        std::snprintf(serialHex, sizeof(serialHex), "%08lX", static_cast<unsigned long>(r.serial));

        mapIni += "[Drive" + std::to_string(i) + "]\r\n";
        mapIni += std::string("Letter=") + static_cast<char>(r.letter) + "\r\n";
        mapIni += std::string("IsOs=") + (r.isOs ? "1" : "0") + "\r\n";
        mapIni += "VolumeGuid=" + WideToUtf8(r.volumeGuid) + "\r\n";
        mapIni += "StartOffset=" + std::to_string(r.startOffset) + "\r\n";
        mapIni += "ExtentLength=" + std::to_string(r.extentLen) + "\r\n";
        mapIni += std::string("VolumeSerial=") + serialHex + "\r\n";
        mapIni += "FileSystem=" + WideToUtf8(r.fsName) + "\r\n\r\n";
    }

    std::wstring driveMapPath = JoinPath(tempDir, L"OneTimePE_DriveMap.ini");
    WriteAsciiFile(driveMapPath, mapIni);

    // 2. Update OneTimePE_Bridge.ini + OneTimePE_Runner.exe
    std::wstring bridgeIni = JoinPath(tempDir, L"OneTimePE_Bridge.ini");
    DeleteFileW(bridgeIni.c_str());
    EnsureUnicodeIni(bridgeIni);
    if (!appBcdRelDir.empty()) {
        WritePrivateProfileStringW(L"Bridge", L"RelAppDir", appBcdRelDir.c_str(), bridgeIni.c_str());
        std::wstring bridgeCmd = JoinPath(tempDir, L"OneTimePE_NetBridge.cmd");
        std::string relOem = WideToOemOrAcp(appBcdRelDir, GetOEMCP());
        WriteAsciiFile(bridgeCmd, "@set \"OTPE_REL=" + relOem + "\"\r\n");
    }
    WritePrivateProfileStringW(L"Bridge", L"AutoApplyNet", autoApplyNetwork ? L"1" : L"0", bridgeIni.c_str());

    // 3. Remote Support (AnySupport / ManualLauncher.exe) settings
    std::wstring safeExe = remoteSupportExe.empty() ? L"ManualLauncher.exe" : remoteSupportExe;
    WritePrivateProfileStringW(L"RemoteSupport", L"Enable", remoteSupportEnable ? L"1" : L"0", bridgeIni.c_str());
    WritePrivateProfileStringW(L"RemoteSupport", L"Code", remoteSupportCode.c_str(), bridgeIni.c_str());
    WritePrivateProfileStringW(L"RemoteSupport", L"ExeName", safeExe.c_str(), bridgeIni.c_str());

    // Export raw code file and environment CMD for custom scripts in WinPE
    std::wstring remoteCodePath = JoinPath(tempDir, L"OneTimePE_RemoteCode.txt");
    WriteAsciiFile(remoteCodePath, WideToUtf8(remoteSupportCode) + "\r\n");

    std::wstring remoteCmd = JoinPath(tempDir, L"OneTimePE_RemoteSupport.cmd");
    std::string codeOem = WideToOemOrAcp(remoteSupportCode, GetOEMCP());
    std::string exeOem  = WideToOemOrAcp(safeExe, GetOEMCP());
    std::string cmdContent = "@set \"ANYSUPPORT_CODE=" + codeOem + "\"\r\n"
                             "@set \"ANYSUPPORT_EXE=" + exeOem + "\"\r\n"
                             "@set \"REMOTE_CODE=" + codeOem + "\"\r\n";
    WriteAsciiFile(remoteCmd, cmdContent);

    wchar_t selfExe[MAX_PATH * 4]{};
    if (GetModuleFileNameW(nullptr, selfExe, _countof(selfExe)) > 0) {
        CopyFileW(selfExe, JoinPath(tempDir, L"OneTimePE_Runner.exe").c_str(), FALSE);
    }

    if (logCb) {
        std::wstring mappedLetters;
        for (size_t i = 0; i < records.size(); ++i) {
            if (i > 0) mappedLetters += L", ";
            mappedLetters += std::wstring(1, records[i].letter) + L":";
        }
        logCb(L"PE 부팅용 내장 드라이브 문자 매핑 기록 완료 (" + mappedLetters + L") -> " + driveMapPath);
        if (remoteSupportEnable && !remoteSupportCode.empty()) {
            logCb(L"애니서포트(원격지원) 자동 연결 설정 기록 완료 [인증번호: " + remoteSupportCode + L", 대상: " + safeExe + L"]");
        }
    }
    return true;
}

bool PatchBootWimUnattended(const std::wstring& appDir,
                            const std::wstring& wimFullPath,
                            const LogCallback& logCb,
                            std::wstring& outNewWimSig) {
    outNewWimSig.clear();
    DWORD wimAttr = GetFileAttributesW(wimFullPath.c_str());
    if (wimAttr == INVALID_FILE_ATTRIBUTES) {
        if (logCb) logCb(L"오류: 패치할 WIM 파일을 찾을 수 없습니다: " + wimFullPath);
        return false;
    }

    // Clear Read-Only attribute (common on files copied from mounted ISO images)
    if (wimAttr & FILE_ATTRIBUTE_READONLY) {
        if (logCb) logCb(L"WIM 파일의 읽기 전용(Read-Only) 속성을 해제합니다...");
        SetFileAttributesW(wimFullPath.c_str(), wimAttr & ~FILE_ATTRIBUTE_READONLY);
    }

    // 0. If the file is actually LZMS/ESD compressed, convert it to a standard mountable/bootable WIM first
    WimInspectionInfo preCheck = InspectWimFile(wimFullPath);
    if (preCheck.validWimMagic && preCheck.isLzmsEsd) {
        if (logCb) {
            logCb(L"알림: ESD(LZMS 고압축) 포맷이 감지되어 먼저 RAMDisk 부팅 및 마운트가 가능한 표준 WIM으로 변환합니다...");
        }
        uint32_t srcIdx = (preCheck.bootIndex > 0) ? preCheck.bootIndex
                                                   : ((preCheck.imageCount > 0) ? preCheck.imageCount : 1u);
        std::wstring convSig;
        if (!ConvertEsdWimToBootableWim(wimFullPath, srcIdx, logCb, convSig)) {
            return false;
        }
    }

    // 1. Inspect WIM image indices via DISM (/English for deterministic parsing)
    if (logCb) logCb(L"--- WIM 자동 드라이브 문자(C:) 복구 및 무인 부팅 패치 시작 ---");
    ExecOutput infoRes = RunHiddenProcess(L"dism.exe",
                                          L"/English /Get-WimInfo /WimFile:\"" + wimFullPath + L"\"",
                                          logCb);
    if (infoRes.exitCode != 0) {
        if (logCb) logCb(L"오류: DISM WIM 이미지 정보를 읽지 못했습니다.");
        return false;
    }

    std::vector<int> indices;
    size_t pos = 0;
    while (pos < infoRes.text.size()) {
        size_t nl = infoRes.text.find(L'\n', pos);
        std::wstring ln = Trim(nl == std::wstring::npos
                               ? infoRes.text.substr(pos)
                               : infoRes.text.substr(pos, nl - pos));
        if (ln.rfind(L"Index :", 0) == 0) {
            int idxVal = _wtoi(Trim(ln.substr(7)).c_str());
            if (idxVal > 0) indices.push_back(idxVal);
        }
        if (nl == std::wstring::npos) break;
        pos = nl + 1;
    }
    if (indices.empty()) indices.push_back(1);

    std::wstring mountDir = JoinPath(appDir, L"_wim_mount_tmp");
    {
        wchar_t winDir[MAX_PATH]{};
        if (GetWindowsDirectoryW(winDir, MAX_PATH) > 0) {
            mountDir = JoinPath(JoinPath(winDir, L"Temp"), L"OneTimePE_WimMount");
        }
    }

    wchar_t selfExeBuf[MAX_PATH * 4]{};
    DWORD selfExeLen = GetModuleFileNameW(nullptr, selfExeBuf, _countof(selfExeBuf));
    std::wstring selfExePath(selfExeBuf, selfExeLen);

    // Clean up any stale mount state before starting
    RunHiddenProcess(L"dism.exe", L"/English /Cleanup-Mountpoints", nullptr);
    if (GetFileAttributesW(mountDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        RunHiddenProcess(L"dism.exe", L"/English /Unmount-Image /MountDir:\"" + mountDir + L"\" /Discard", nullptr);
        DeleteDirectoryContentsRecursive(mountDir);
        RemoveDirectoryW(mountDir.c_str());
    }
    EnsureDirectoryRecursive(mountDir);

    // Self-contained RAMDisk bootstrapper placed inside X:\Windows\System32\OneTimePE_BootInit.cmd
    // 1) Restores C: drive via OneTimePE_Helper.exe, 2) Delegates to latest OneTimePE_Runner.exe on host drive if present
    const std::string bootInitScript =
        "@echo off\r\n"
        "setlocal\r\n"
        "if exist \"X:\\Windows\\Temp\\OneTimePE_BootInitDone.flg\" exit /b 0\r\n"
        "if not exist \"X:\\Windows\\Temp\" mkdir \"X:\\Windows\\Temp\" >nul 2>&1\r\n"
        "if exist \"%SystemRoot%\\System32\\OneTimePE_Helper.exe\" (\r\n"
        "    \"%SystemRoot%\\System32\\OneTimePE_Helper.exe\" --pe-fix-drives\r\n"
        ") else if exist \"X:\\Windows\\System32\\OneTimePE_Helper.exe\" (\r\n"
        "    \"X:\\Windows\\System32\\OneTimePE_Helper.exe\" --pe-fix-drives\r\n"
        ")\r\n"
        "for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (\r\n"
        "    if exist \"%%D:\\Windows\\Temp\\OneTimePE_Runner.exe\" (\r\n"
        "        start \"\" /min \"%%D:\\Windows\\Temp\\OneTimePE_Runner.exe\" --pe-boot-init\r\n"
        "        exit /b 0\r\n"
        "    )\r\n"
        ")\r\n"
        "if exist \"%SystemRoot%\\System32\\OneTimePE_Helper.exe\" (\r\n"
        "    start \"\" /min \"%SystemRoot%\\System32\\OneTimePE_Helper.exe\" --pe-boot-init\r\n"
        "    exit /b 0\r\n"
        ")\r\n"
        "if exist \"X:\\Windows\\System32\\OneTimePE_Helper.exe\" (\r\n"
        "    start \"\" /min \"X:\\Windows\\System32\\OneTimePE_Helper.exe\" --pe-boot-init\r\n"
        "    exit /b 0\r\n"
        ")\r\n"
        "endlocal\r\n"
        "exit /b 0\r\n";

    const std::string patchMetaIni =
        "[WimPatch]\r\n"
        "Version=3\r\n"
        "FixDrives=1\r\n"
        "AutoNetSupport=1\r\n"
        "RemoteSupport=1\r\n";

    for (int idx : indices) {
        if (logCb) {
            logCb(L"WIM 이미지 마운트 중 (Index " + std::to_wstring(idx) + L"/" +
                  std::to_wstring(indices.size()) + L") — 잠시만 기다려 주세요...");
        }

        std::wstring mountArgs = L"/English /Mount-Image /ImageFile:\"" + wimFullPath +
                                 L"\" /Index:" + std::to_wstring(idx) +
                                 L" /MountDir:\"" + mountDir + L"\"";
        ExecOutput mr = RunHiddenProcess(L"dism.exe", mountArgs, logCb, true);
        if (mr.exitCode != 0) {
            if (logCb) logCb(L"오류: WIM 마운트 실패 (Index " + std::to_wstring(idx) + L")");
            RemoveDirectoryW(mountDir.c_str());
            return false;
        }

        std::wstring winMountDir = JoinPath(mountDir, L"Windows");
        std::wstring sys32Dir    = JoinPath(winMountDir, L"System32");
        bool patchOk = true;

        // A. Embed standalone OneTimePE_Helper.exe + OneTimePE_BootInit.cmd + OneTimePE_WimPatch.ini directly in X:\Windows\System32
        std::wstring helperDst = JoinPath(sys32Dir, L"OneTimePE_Helper.exe");
        DWORD hAttr = GetFileAttributesW(helperDst.c_str());
        if (hAttr != INVALID_FILE_ATTRIBUTES && (hAttr & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesW(helperDst.c_str(), hAttr & ~FILE_ATTRIBUTE_READONLY);
        }
        if (selfExePath.empty() || !CopyFileW(selfExePath.c_str(), helperDst.c_str(), FALSE)) {
            if (logCb) logCb(L"오류: OneTimePE_Helper.exe 내장 복사 실패");
            patchOk = false;
        } else if (logCb) {
            logCb(L" • 주입 완료: \\Windows\\System32\\OneTimePE_Helper.exe (C: 드라이브 자동복구 & 무인 엔진)");
        }

        if (patchOk) {
            std::wstring bootInitPath = JoinPath(sys32Dir, L"OneTimePE_BootInit.cmd");
            std::wstring autoNetPath  = JoinPath(sys32Dir, L"OneTimePE_AutoNet.cmd");
            std::wstring patchIniPath = JoinPath(sys32Dir, L"OneTimePE_WimPatch.ini");
            if (!WriteAsciiFile(bootInitPath, bootInitScript) ||
                !WriteAsciiFile(autoNetPath, bootInitScript) ||
                !WriteAsciiFile(patchIniPath, patchMetaIni)) {
                if (logCb) logCb(L"오류: 부팅 초기화 스크립트 기록 실패");
                patchOk = false;
            } else if (logCb) {
                logCb(L" • 주입 완료: \\Windows\\System32\\OneTimePE_BootInit.cmd");
            }
        }

        // B. Hook 1: \Windows\System32\startnet.cmd (Standard WinPE)
        if (patchOk) {
            std::wstring startnetPath = JoinPath(sys32Dir, L"startnet.cmd");
            if (GetFileAttributesW(startnetPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                WriteAsciiFile(startnetPath,
                               "wpeinit\r\n@start \"\" /min \"%SystemRoot%\\System32\\OneTimePE_BootInit.cmd\"\r\n");
                if (logCb) logCb(L" • 생성 및 후킹 완료: \\Windows\\System32\\startnet.cmd");
            } else {
                std::wstring content;
                TextEncoding enc = TextEncoding::AsciiOrUtf8NoBom;
                if (ReadTextAutoEncoding(startnetPath, content, enc)) {
                    std::wstring low = ToLower(content);
                    if (low.find(L"onetimepe_bootinit.cmd") == std::wstring::npos &&
                        low.find(L"onetimepe_autonet.cmd") == std::wstring::npos) {
                        if (!content.empty() && content.back() != L'\n') content += L"\r\n";
                        content += L"@start \"\" /min \"%SystemRoot%\\System32\\OneTimePE_BootInit.cmd\"\r\n";
                        WriteTextWithEncoding(startnetPath, content, enc);
                        if (logCb) logCb(L" • 후킹 완료: \\Windows\\System32\\startnet.cmd");
                    } else if (logCb) {
                        logCb(L" • 확인됨: startnet.cmd에 이미 부팅 훅이 존재함");
                    }
                }
            }
        }

        // C. Hook 2: \Windows\System32\winpeshl.ini (Custom GUI PE — ONLY if file already exists!)
        std::wstring winpeshlPath = JoinPath(sys32Dir, L"winpeshl.ini");
        if (patchOk && GetFileAttributesW(winpeshlPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring iniContent;
            TextEncoding enc = TextEncoding::AsciiOrUtf8NoBom;
            if (ReadTextAutoEncoding(winpeshlPath, iniContent, enc)) {
                std::wstring lowIni = ToLower(iniContent);
                if (lowIni.find(L"onetimepe_bootinit.cmd") == std::wstring::npos &&
                    lowIni.find(L"onetimepe_autonet.cmd") == std::wstring::npos) {
                    size_t secPos = lowIni.find(L"[launchapps]");
                    const std::wstring hookLine =
                        L"%SYSTEMROOT%\\System32\\cmd.exe, /c start \"\" /min \"%SystemRoot%\\System32\\OneTimePE_BootInit.cmd\"\r\n";
                    if (secPos != std::wstring::npos) {
                        size_t lineEnd = iniContent.find(L'\n', secPos);
                        if (lineEnd == std::wstring::npos) {
                            iniContent += L"\r\n" + hookLine;
                        } else {
                            iniContent.insert(lineEnd + 1, hookLine);
                        }
                    } else {
                        if (!iniContent.empty() && iniContent.back() != L'\n') iniContent += L"\r\n";
                        iniContent += L"[LaunchApps]\r\n" + hookLine;
                    }
                    WriteTextWithEncoding(winpeshlPath, iniContent, enc);
                    if (logCb) logCb(L" • 후킹 완료: \\Windows\\System32\\winpeshl.ini ([LaunchApps] 비동기 연결)");
                } else if (logCb) {
                    logCb(L" • 확인됨: winpeshl.ini에 이미 부팅 훅이 존재함");
                }
            }
        }

        // D. Hook 3: PECMD.INI in System32 or Windows (covers Custom PEs launched directly via Setup\CmdLine -> PECMD.EXE!)
        if (patchOk) {
            for (const std::wstring& pecmdCandidate : {
                     JoinPath(sys32Dir, L"PECMD.INI"),
                     JoinPath(winMountDir, L"PECMD.INI") }) {
                if (GetFileAttributesW(pecmdCandidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    bool already = false;
                    if (PrependLinePreservingRawEncoding(
                            pecmdCandidate,
                            "EXEC !%SystemRoot%\\System32\\OneTimePE_BootInit.cmd\r\n",
                            "onetimepe_bootinit.cmd",
                            already)) {
                        std::wstring relDisp = pecmdCandidate.substr(mountDir.size());
                        if (logCb) {
                            logCb(already ? (L" • 확인됨: " + relDisp + L"에 이미 PECMD 훅이 존재함")
                                          : (L" • 후킹 완료: " + relDisp + L" (원본 인코딩 무손실 보존)"));
                        }
                    }
                }
            }
        }

        // E. Hook 4: Explorer Startup folder fallback for custom Explorer-shell PEs
        std::wstring startupDir = JoinPath(mountDir, L"Users\\Default\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup");
        if (patchOk && GetFileAttributesW(startupDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(JoinPath(startupDir, L"OneTimePE_AutoNet.cmd").c_str());
            WriteAsciiFile(JoinPath(startupDir, L"OneTimePE_BootInit.cmd"), bootInitScript);
            if (logCb) logCb(L" • 후킹 완료: Default User Startup 시작프로그램 폴더");
        }

        // F. Ensure both System32\winload.{efi,exe} and System32\Boot\winload.{efi,exe} exist when one is present
        //    Prevents 0xc000000f loader path mismatch across custom / recovery WinPE builds!
        if (patchOk) {
            std::wstring bootSubDir = JoinPath(sys32Dir, L"Boot");
            for (const wchar_t* loaderFile : { L"winload.efi", L"winload.exe" }) {
                std::wstring pSys32 = JoinPath(sys32Dir, loaderFile);
                std::wstring pBoot  = JoinPath(bootSubDir, loaderFile);
                bool hasSys32 = (GetFileAttributesW(pSys32.c_str()) != INVALID_FILE_ATTRIBUTES);
                bool hasBoot  = (GetFileAttributesW(pBoot.c_str()) != INVALID_FILE_ATTRIBUTES);
                if (!hasSys32 && hasBoot) {
                    if (CopyFileW(pBoot.c_str(), pSys32.c_str(), TRUE)) {
                        if (logCb) logCb(std::wstring(L" • 부트 로더 경로 보정: Boot\\") + loaderFile + L" -> System32\\" + loaderFile);
                    }
                } else if (hasSys32 && !hasBoot && GetFileAttributesW(bootSubDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    CopyFileW(pSys32.c_str(), pBoot.c_str(), TRUE);
                }
            }
        }

        // Unmount & Commit (or Discard on failure)
        if (logCb) {
            logCb(patchOk ? L"변경 사항을 WIM 이미지에 저장(Commit) 및 언마운트 중..."
                          : L"오류 발생으로 마운트를 롤백(Discard) 중...");
        }
        std::wstring unmountArgs = L"/English /Unmount-Image /MountDir:\"" + mountDir + L"\" " +
                                   (patchOk ? L"/Commit" : L"/Discard");
        ExecOutput ur = RunHiddenProcess(L"dism.exe", unmountArgs, logCb, true);
        if (!patchOk || ur.exitCode != 0) {
            if (logCb) logCb(L"오류: WIM 언마운트/저장 중 문제가 발생했습니다.");
            DeleteDirectoryContentsRecursive(mountDir);
            RemoveDirectoryW(mountDir.c_str());
            return false;
        }
    }

    DeleteDirectoryContentsRecursive(mountDir);
    RemoveDirectoryW(mountDir.c_str());

    // Ensure WIM header has BootIndex > 0 so bootmgfw.efi can mount it as RAMDisk without 0xc000000f
    WimInspectionInfo postCheck = InspectWimFile(wimFullPath);
    if (postCheck.validWimMagic && postCheck.bootIndex == 0 && !indices.empty()) {
        uint32_t targetBootIdx = static_cast<uint32_t>(indices.back());
        TrySetWimBootIndexViaWimgapi(wimFullPath, targetBootIdx);
        if (logCb) {
            logCb(L" • WIM 부팅 가능 플래그(BootIndex=" + std::to_wstring(targetBootIdx) + L") 보정 적용");
        }
    }

    outNewWimSig = ComputeFileSignature(wimFullPath);
    // Warm patch cache with new signature
    IsWimUnattendedPatched(wimFullPath, postCheck.bootIndex > 0 ? postCheck.bootIndex : 1u);
    if (logCb) logCb(L"★ WIM 드라이브 문자(C:) 자동 복구 및 무인 부팅 패치가 완료되었습니다!");
    return true;
}

// ============================================================================
// Silent WinPE Drive-Letter Auto-Recovery Engine (Solves Case B automatically)
// ============================================================================
namespace {

struct PePartitionLayoutEntry {
    DWORD     diskNumber     = DWORD(-1);
    DWORD     partNumber     = DWORD(-1);
    LONGLONG  startOffset    = -1;
    LONGLONG  partLength     = 0;
    bool      isGpt          = false;
    GUID      gptType        = {};
    ULONGLONG gptAttrs       = 0;
    BYTE      mbrType        = 0;
    bool      isSystemOrRec  = false;
};

struct PeVolumeEntry {
    std::wstring              volumeGuid;    // trailing backslash, lowercase: "\\?\volume{...}\"
    std::wstring              volNoSlash;    // no trailing backslash
    DWORD                     diskNumber     = DWORD(-1);
    DWORD                     partNumber     = DWORD(-1);
    LONGLONG                  startOffset    = -1;
    LONGLONG                  extentLen      = 0;
    DWORD                     serial         = 0;
    std::wstring              fsName;
    bool                      isInternal     = true;
    bool                      isSystemOrRec  = false;
    bool                      isWindowsOs    = false;
    bool                      hasDriveMapIni = false;
    ULONGLONG                 osHiveTime     = 0;
    std::vector<wchar_t>      mountedLetters;
};

struct RecordedDriveTarget {
    wchar_t      letter      = 0;
    bool         isOs        = false;
    std::wstring volumeGuid;
    LONGLONG     startOffset = -1;
    LONGLONG     extentLen   = 0;
    DWORD        serial      = 0;
};

void EnablePeAutoMount(const LogCallback& logCb) {
    HANDLE hMm = CreateFileW(L"\\\\.\\MountPointManager",
                             GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (hMm != INVALID_HANDLE_VALUE) {
        BOOL enable = TRUE;
        DWORD ret = 0;
        if (DeviceIoControl(hMm, IOCTL_MOUNTMGR_SET_AUTO_MOUNT, &enable, sizeof(enable),
                            nullptr, 0, &ret, nullptr)) {
            if (logCb) logCb(L"MountMgr AutoMount enabled via IOCTL.");
        }
        CloseHandle(hMm);
    }
    RunHiddenProcess(L"mountvol.exe", L"/e", nullptr);
}

int ClearInternalDisksOfflineAndReadOnly(const LogCallback& logCb) {
    int internalDiskCount = 0;
    for (DWORD d = 0; d < 16; ++d) {
        std::wstring phys = L"\\\\.\\PhysicalDrive" + std::to_wstring(d);
        STORAGE_BUS_TYPE bt = QueryDeviceBusType(phys);
        if (bt == BusTypeUnknown) {
            HANDLE hProbe = CreateFileW(phys.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, 0, nullptr);
            if (hProbe == INVALID_HANDLE_VALUE) continue;
            CloseHandle(hProbe);
        }
        if (!IsInternalFixedBus(bt)) continue;
        ++internalDiskCount;

        HANDLE hDisk = CreateFileW(phys.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (hDisk == INVALID_HANDLE_VALUE) continue;

        OtpeGetDiskAttrs gda{};
        gda.version = sizeof(gda);
        DWORD ret = 0;
        if (DeviceIoControl(hDisk, IOCTL_DISK_GET_DISK_ATTRIBUTES, nullptr, 0,
                            &gda, sizeof(gda), &ret, nullptr)) {
            const ULONGLONG blockBits = 0x3ULL; // DISK_ATTRIBUTE_OFFLINE (1) | DISK_ATTRIBUTE_READ_ONLY (2)
            if ((gda.attributes & blockBits) != 0) {
                OtpeSetDiskAttrs sda{};
                sda.version        = sizeof(sda);
                sda.persist        = TRUE;
                sda.attributes     = 0;
                sda.attributesMask = blockBits;
                if (DeviceIoControl(hDisk, IOCTL_DISK_SET_DISK_ATTRIBUTES, &sda, sizeof(sda),
                                    nullptr, 0, &ret, nullptr)) {
                    DeviceIoControl(hDisk, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &ret, nullptr);
                    if (logCb) {
                        logCb(L"Cleared Offline/ReadOnly attributes on PhysicalDrive" + std::to_wstring(d));
                    }
                }
            }
        }
        CloseHandle(hDisk);
    }
    return internalDiskCount;
}

std::vector<PePartitionLayoutEntry> ScanInternalPartitionLayouts() {
    std::vector<PePartitionLayoutEntry> out;
    std::vector<BYTE> buf(32768);

    for (DWORD d = 0; d < 16; ++d) {
        std::wstring phys = L"\\\\.\\PhysicalDrive" + std::to_wstring(d);
        STORAGE_BUS_TYPE bt = QueryDeviceBusType(phys);
        if (!IsInternalFixedBus(bt)) continue;

        HANDLE hDisk = CreateFileW(phys.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (hDisk == INVALID_HANDLE_VALUE) continue;

        DWORD ret = 0;
        if (DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0,
                            buf.data(), static_cast<DWORD>(buf.size()), &ret, nullptr) &&
            ret >= sizeof(DRIVE_LAYOUT_INFORMATION_EX)) {
            const auto* layout = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());
            DWORD maxParts = std::min<DWORD>(layout->PartitionCount, 128);
            for (DWORD i = 0; i < maxParts; ++i) {
                const PARTITION_INFORMATION_EX& pe = layout->PartitionEntry[i];
                if (pe.PartitionLength.QuadPart <= 0 || pe.PartitionNumber == 0) continue;

                PePartitionLayoutEntry item{};
                item.diskNumber  = d;
                item.partNumber  = pe.PartitionNumber;
                item.startOffset = pe.StartingOffset.QuadPart;
                item.partLength  = pe.PartitionLength.QuadPart;

                if (pe.PartitionStyle == PARTITION_STYLE_GPT) {
                    item.isGpt    = true;
                    item.gptType  = pe.Gpt.PartitionType;
                    item.gptAttrs = pe.Gpt.Attributes;
                    // Anything other than Basic Data (ESP, MSR, WinRE, OEM Recovery GUID) is system/recovery!
                    item.isSystemOrRec = !GuidEq(pe.Gpt.PartitionType, kGptTypeBasicData);
                } else if (pe.PartitionStyle == PARTITION_STYLE_MBR) {
                    if (!pe.Mbr.RecognizedPartition || pe.Mbr.PartitionType == 0) continue;
                    item.mbrType = pe.Mbr.PartitionType;
                    item.isSystemOrRec = (pe.Mbr.PartitionType == 0xEF ||
                                          pe.Mbr.PartitionType == 0x27 ||
                                          pe.Mbr.PartitionType == 0x12 ||
                                          pe.Mbr.PartitionType == 0xDE ||
                                          pe.Mbr.PartitionType == 0x84);
                }
                out.push_back(item);
            }
        }
        CloseHandle(hDisk);
    }
    return out;
}

bool ClearVolumeBlockingAttributesWin32(const std::wstring& volNoSlash, const LogCallback& logCb) {
    HANDLE hVol = CreateFileW(volNoSlash.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) return false;

    DWORD ret = 0;
    DeviceIoControl(hVol, IOCTL_VOLUME_ONLINE, nullptr, 0, nullptr, 0, &ret, nullptr);

    OtpeVolGetGptAttrs cur{};
    if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_GPT_ATTRIBUTES, nullptr, 0,
                        &cur, sizeof(cur), &ret, nullptr)) {
        const ULONGLONG clearMask = GPT_BASIC_DATA_ATTRIBUTE_READ_ONLY |
                                    GPT_BASIC_DATA_ATTRIBUTE_HIDDEN |
                                    GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER;
        if ((cur.gptAttributes & clearMask) != 0) {
            OtpeVolSetGptAttrs setReq{};
            setReq.gptAttributes              = (cur.gptAttributes & ~clearMask);
            setReq.revertOnClose              = FALSE;
            setReq.applyToAllConnectedVolumes = FALSE;
            if (DeviceIoControl(hVol, IOCTL_VOLUME_SET_GPT_ATTRIBUTES, &setReq, sizeof(setReq),
                                nullptr, 0, &ret, nullptr)) {
                if (logCb) logCb(L"Cleared GPT Hidden/NoDefaultDriveLetter/ReadOnly on " + volNoSlash);
            }
            DeviceIoControl(hVol, IOCTL_VOLUME_ONLINE, nullptr, 0, nullptr, 0, &ret, nullptr);
        }
    }

    CloseHandle(hVol);
    return true;
}

bool ParseAsciiDriveMapIni(const std::wstring& iniPathOnVol,
                           wchar_t& outOsLetter,
                           std::vector<RecordedDriveTarget>& outTargets) {
    HANDLE h = CreateFileW(iniPathOnVol.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, nullptr);
    if (sz == INVALID_FILE_SIZE || sz == 0 || sz > 65536) {
        CloseHandle(h);
        return false;
    }
    std::string raw(sz, '\0');
    DWORD rd = 0;
    ReadFile(h, raw.data(), sz, &rd, nullptr);
    CloseHandle(h);
    raw.resize(rd);

    outOsLetter = L'C';
    outTargets.clear();

    std::map<std::string, std::map<std::string, std::string>> secs;
    std::string curSec;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t nl = raw.find('\n', pos);
        std::string ln = (nl == std::string::npos) ? raw.substr(pos) : raw.substr(pos, nl - pos);
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ' || ln.back() == '\t')) ln.pop_back();
        size_t st = 0;
        while (st < ln.size() && (ln[st] == ' ' || ln[st] == '\t')) ++st;
        ln = ln.substr(st);

        if (ln.size() >= 2 && ln.front() == '[' && ln.back() == ']') {
            curSec = ln.substr(1, ln.size() - 2);
        } else if (!ln.empty() && ln.front() != ';') {
            size_t eq = ln.find('=');
            if (eq != std::string::npos) {
                secs[curSec][ln.substr(0, eq)] = ln.substr(eq + 1);
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    auto dmIt = secs.find("DriveMap");
    if (dmIt == secs.end()) return false;

    auto osLtIt = dmIt->second.find("OsLetter");
    if (osLtIt != dmIt->second.end() && !osLtIt->second.empty()) {
        char c = osLtIt->second[0];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        if (c >= 'A' && c <= 'Z' && c != 'X') outOsLetter = static_cast<wchar_t>(c);
    }

    int count = 0;
    auto cntIt = dmIt->second.find("Count");
    if (cntIt != dmIt->second.end()) {
        try { count = std::stoi(cntIt->second); }
        catch (...) { count = atoi(cntIt->second.c_str()); }
    }

    for (int i = 0; i < count && i < 26; ++i) {
        auto sIt = secs.find("Drive" + std::to_string(i));
        if (sIt == secs.end()) continue;
        const auto& kv = sIt->second;

        auto ltIt = kv.find("Letter");
        if (ltIt == kv.end() || ltIt->second.empty()) continue;
        char c = ltIt->second[0];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        if (c < 'A' || c > 'Z' || c == 'X') continue;

        RecordedDriveTarget t{};
        t.letter = static_cast<wchar_t>(c);
        auto osIt = kv.find("IsOs");
        t.isOs = (osIt != kv.end() && osIt->second == "1");
        auto vgIt = kv.find("VolumeGuid");
        if (vgIt != kv.end()) {
            t.volumeGuid = ToLower(AnsiToWide(vgIt->second.c_str()));
            if (!t.volumeGuid.empty() && t.volumeGuid.back() != L'\\') t.volumeGuid += L'\\';
        }
        auto soIt = kv.find("StartOffset");
        if (soIt != kv.end()) {
            try { t.startOffset = std::stoll(soIt->second); }
            catch (...) { t.startOffset = strtoll(soIt->second.c_str(), nullptr, 10); }
        }
        auto elIt = kv.find("ExtentLength");
        if (elIt != kv.end()) {
            try { t.extentLen = std::stoll(elIt->second); }
            catch (...) { t.extentLen = strtoll(elIt->second.c_str(), nullptr, 10); }
        }
        auto srIt = kv.find("VolumeSerial");
        if (srIt != kv.end()) {
            try { t.serial = static_cast<DWORD>(std::stoul(srIt->second, nullptr, 16)); }
            catch (...) { t.serial = static_cast<DWORD>(strtoul(srIt->second.c_str(), nullptr, 16)); }
        }
        outTargets.push_back(std::move(t));
    }
    return !outTargets.empty();
}

std::vector<PeVolumeEntry> EnumeratePeVolumes(const std::vector<PePartitionLayoutEntry>& layouts,
                                              wchar_t& ioOsLetter,
                                              std::vector<RecordedDriveTarget>& ioRecordedTargets) {
    std::vector<PeVolumeEntry> vols;
    wchar_t volBuf[MAX_PATH]{};
    HANDLE hFind = FindFirstVolumeW(volBuf, _countof(volBuf));
    if (hFind == INVALID_HANDLE_VALUE) return vols;

    ULONGLONG newestMapTime = 0;

    do {
        std::wstring vGuid = ToLower(volBuf);
        if (vGuid.empty()) continue;
        if (vGuid.back() != L'\\') vGuid += L'\\';
        std::wstring vNoSlash = vGuid.substr(0, vGuid.size() - 1);

        UINT dt = GetDriveTypeW(vGuid.c_str());
        if (dt == DRIVE_CDROM || dt == DRIVE_REMOTE) continue;

        PeVolumeEntry ve{};
        ve.volumeGuid = vGuid;
        ve.volNoSlash = vNoSlash;

        wchar_t pathsBuf[512]{};
        DWORD retChars = 0;
        bool isXRamdisk = false;
        if (GetVolumePathNamesForVolumeNameW(vGuid.c_str(), pathsBuf, _countof(pathsBuf), &retChars)) {
            const wchar_t* p = pathsBuf;
            while (*p != L'\0') {
                if (std::iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'\0')) {
                    wchar_t L = static_cast<wchar_t>(std::towupper(p[0]));
                    if (L == L'X') isXRamdisk = true;
                    ve.mountedLetters.push_back(L);
                }
                p += wcslen(p) + 1;
            }
        }
        if (isXRamdisk) continue;

        STORAGE_BUS_TYPE bt = QueryDeviceBusType(vNoSlash);
        ve.isInternal = IsInternalFixedBus(bt) && (dt != DRIVE_REMOVABLE);

        QueryVolumePrimaryExtent(vNoSlash, ve.diskNumber, ve.startOffset, ve.extentLen);

        STORAGE_DEVICE_NUMBER sdn{};
        if (QueryDeviceNumberForHandlePath(vNoSlash, sdn) && sdn.DeviceType == FILE_DEVICE_DISK) {
            if (ve.diskNumber == DWORD(-1)) ve.diskNumber = sdn.DeviceNumber;
            ve.partNumber = sdn.PartitionNumber;
        }

        // Cross-reference partition layout table
        for (const auto& le : layouts) {
            if (le.diskNumber == ve.diskNumber && le.startOffset == ve.startOffset) {
                if (ve.partNumber == DWORD(-1) || ve.partNumber == 0) ve.partNumber = le.partNumber;
                if (le.isSystemOrRec) ve.isSystemOrRec = true;
                break;
            }
        }

        // Online internal Basic Data volumes so their root directory is readable even without a drive letter
        if (ve.isInternal && !ve.isSystemOrRec) {
            HANDLE hVol = CreateFileW(vNoSlash.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
            if (hVol != INVALID_HANDLE_VALUE) {
                DWORD dummy = 0;
                DeviceIoControl(hVol, IOCTL_VOLUME_ONLINE, nullptr, 0, nullptr, 0, &dummy, nullptr);
                CloseHandle(hVol);
            }
        }

        wchar_t fsBuf[64]{};
        DWORD serial = 0;
        if (GetVolumeInformationW(vGuid.c_str(), nullptr, 0, &serial, nullptr, nullptr, fsBuf, _countof(fsBuf))) {
            ve.serial = serial;
            ve.fsName = fsBuf;
        }

        // Detect disguised WinRE / OEM Recovery partitions (< 25 GB with WinRE/Samsung_Recovery and no explorer.exe)
        bool hasSysHive  = FileExistsRegular(vGuid + L"Windows\\System32\\config\\SYSTEM");
        bool hasSoftHive = FileExistsRegular(vGuid + L"Windows\\System32\\config\\SOFTWARE");
        bool hasExplorer = FileExistsRegular(vGuid + L"Windows\\explorer.exe") ||
                           FileExistsRegular(vGuid + L"Windows\\System32\\ntoskrnl.exe");

        if (!hasExplorer && ve.extentLen > 0 && ve.extentLen < (25LL * 1024LL * 1024LL * 1024LL)) {
            if (FileExistsRegular(vGuid + L"Recovery\\WindowsRE\\winre.wim") ||
                DirExists(vGuid + L"Samsung_Recovery") ||
                DirExists(vGuid + L"RecoveryImage")) {
                ve.isSystemOrRec = true;
            }
        }

        if (ve.isInternal && !ve.isSystemOrRec && hasSysHive && hasSoftHive && hasExplorer) {
            ve.isWindowsOs = true;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW((vGuid + L"Windows\\System32\\config\\SYSTEM").c_str(),
                                     GetFileExInfoStandard, &fad)) {
                ve.osHiveTime = (static_cast<ULONGLONG>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                                fad.ftLastWriteTime.dwLowDateTime;
            }
        }

        std::wstring mapIniPath = vGuid + L"Windows\\Temp\\OneTimePE_DriveMap.ini";
        WIN32_FILE_ATTRIBUTE_DATA mapFad{};
        if (GetFileAttributesExW(mapIniPath.c_str(), GetFileExInfoStandard, &mapFad) &&
            !(mapFad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            ve.hasDriveMapIni = true;
            ULONGLONG mt = (static_cast<ULONGLONG>(mapFad.ftLastWriteTime.dwHighDateTime) << 32) |
                           mapFad.ftLastWriteTime.dwLowDateTime;
            if (mt >= newestMapTime) {
                wchar_t parsedOs = L'C';
                std::vector<RecordedDriveTarget> parsedTargets;
                if (ParseAsciiDriveMapIni(mapIniPath, parsedOs, parsedTargets)) {
                    newestMapTime     = mt;
                    ioOsLetter        = parsedOs;
                    ioRecordedTargets = std::move(parsedTargets);
                }
            }
        }

        vols.push_back(std::move(ve));
    } while (FindNextVolumeW(hFind, volBuf, _countof(volBuf)));

    FindClose(hFind);
    return vols;
}

bool RunDiskpartScript(const std::string& scriptContent, const LogCallback& logCb) {
    EnsureDirectoryRecursive(L"X:\\Windows\\Temp");
    std::wstring scriptPath = L"X:\\Windows\\Temp\\otpe_diskpart.txt";
    if (!WriteAsciiFile(scriptPath, scriptContent)) return false;
    ExecOutput r = RunHiddenProcess(L"diskpart.exe", L"/s \"" + scriptPath + L"\"", logCb);
    DeleteFileW(scriptPath.c_str());
    return r.exitCode == 0;
}

void NotifyShellDriveChanges(const std::vector<wchar_t>& changedLetters) {
    HMODULE hShell = GetModuleHandleW(L"shell32.dll");
    if (!hShell) hShell = LoadLibraryW(L"shell32.dll");
    if (hShell) {
        using FnSHChangeNotify = void(WINAPI*)(LONG, UINT, LPCVOID, LPCVOID);
        auto pNotify = reinterpret_cast<FnSHChangeNotify>(
            reinterpret_cast<void*>(GetProcAddress(hShell, "SHChangeNotify")));
        if (pNotify) {
            constexpr LONG kShcneDriveAdd  = 0x00000100L;
            constexpr LONG kShcneUpdateDir = 0x00001000L;
            constexpr UINT kShcnfPathW     = 0x0005u;
            for (wchar_t ch : changedLetters) {
                wchar_t root[4] = { ch, L':', L'\\', L'\0' };
                pNotify(kShcneDriveAdd, kShcnfPathW, root, nullptr);
                pNotify(kShcneUpdateDir, kShcnfPathW, root, nullptr);
            }
        }
    }
    DWORD_PTR msgRes = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_DEVICECHANGE, 0x0007 /*DBT_DEVNODES_CHANGED*/,
                        0, SMTO_ABORTIFHUNG, 400, &msgRes);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 400, &msgRes);
}

wchar_t PickSafeFallbackLetter(const std::set<wchar_t>& reservedLetters) {
    // Prefer S..W, R..M — intentionally avoid C..E, X (RAMDisk), and Y (Korean Custom PE PETools/Y.ini)!
    constexpr wchar_t kCandidates[] = {
        L'S', L'T', L'U', L'V', L'W', L'R', L'Q', L'P', L'O', L'N', L'M', L'L', L'K', L'Z'
    };
    DWORD usedMask = GetLogicalDrives();
    for (wchar_t c : kCandidates) {
        if (reservedLetters.count(c) > 0) continue;
        if ((usedMask & (1u << (c - L'A'))) == 0) return c;
    }
    return 0;
}

bool ApplySingleDriveLetterRestore(const PeVolumeEntry& targetVol,
                                   wchar_t targetLetter,
                                   bool verifyWindowsDir,
                                   const std::vector<PeVolumeEntry>& allVols,
                                   const std::set<wchar_t>& reservedLetters,
                                   const LogCallback& logCb) {
    wchar_t targetRoot[4] = { targetLetter, L':', L'\\', L'\0' };

    auto isProperlyMountedNow = [&]() -> bool {
        wchar_t curGuid[128]{};
        if (!GetVolumeNameForVolumeMountPointW(targetRoot, curGuid, _countof(curGuid))) {
            return false;
        }
        std::wstring lowCur = ToLower(curGuid);
        if (!lowCur.empty() && lowCur.back() != L'\\') lowCur += L'\\';
        if (lowCur != targetVol.volumeGuid) return false;
        if (verifyWindowsDir) {
            std::wstring checkSys = std::wstring(targetRoot) + L"Windows\\System32\\config\\SYSTEM";
            return GetFileAttributesW(checkSys.c_str()) != INVALID_FILE_ATTRIBUTES;
        }
        return GetDriveTypeW(targetRoot) == DRIVE_FIXED;
    };

    if (isProperlyMountedNow()) {
        if (logCb) {
            logCb(std::wstring(L"Drive ") + targetLetter + L": is already mounted to target volume " + targetVol.volumeGuid);
        }
        return true;
    }

    // 1. Check if another volume is currently occupying targetLetter:
    DWORD occupantDisk = DWORD(-1);
    DWORD occupantPart = DWORD(-1);
    std::wstring occupantGuid;
    bool occupantIsSystemOrRec = false;

    wchar_t occBuf[128]{};
    if (GetVolumeNameForVolumeMountPointW(targetRoot, occBuf, _countof(occBuf))) {
        occupantGuid = ToLower(occBuf);
        if (!occupantGuid.empty() && occupantGuid.back() != L'\\') occupantGuid += L'\\';
        if (occupantGuid != targetVol.volumeGuid) {
            for (const auto& v : allVols) {
                if (v.volumeGuid == occupantGuid) {
                    occupantDisk          = v.diskNumber;
                    occupantPart          = v.partNumber;
                    occupantIsSystemOrRec = v.isSystemOrRec;
                    break;
                }
            }
            if (logCb) {
                logCb(std::wstring(L"Evicting non-target volume from ") + targetLetter + L": -> " + occupantGuid);
            }
            DeleteVolumeMountPointW(targetRoot);
            if (!occupantIsSystemOrRec && !occupantGuid.empty()) {
                wchar_t fallback = PickSafeFallbackLetter(reservedLetters);
                if (fallback != 0) {
                    wchar_t fbRoot[4] = { fallback, L':', L'\\', L'\0' };
                    if (SetVolumeMountPointW(fbRoot, occupantGuid.c_str()) && logCb) {
                        logCb(std::wstring(L"Reassigned displaced volume to ") + fallback + L":");
                    }
                }
            }
        }
    }

    // 2. Remove any wrong letter currently attached to targetVol (e.g. if Windows partition got D: or E:)
    wchar_t curPaths[512]{};
    DWORD retChars = 0;
    if (GetVolumePathNamesForVolumeNameW(targetVol.volumeGuid.c_str(), curPaths, _countof(curPaths), &retChars)) {
        const wchar_t* p = curPaths;
        while (*p != L'\0') {
            if (std::iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'\0')) {
                wchar_t existingL = static_cast<wchar_t>(std::towupper(p[0]));
                if (existingL != targetLetter && existingL != L'X') {
                    wchar_t oldRoot[4] = { existingL, L':', L'\\', L'\0' };
                    DeleteVolumeMountPointW(oldRoot);
                }
            }
            p += wcslen(p) + 1;
        }
    }

    // 3. Fast Win32 IOCTL unblock + mount attempt
    ClearVolumeBlockingAttributesWin32(targetVol.volNoSlash, logCb);
    SetVolumeMountPointW(targetRoot, targetVol.volumeGuid.c_str());

    if (isProperlyMountedNow()) {
        if (logCb) {
            logCb(std::wstring(L"Successfully restored ") + targetLetter + L": via Win32 Volume API -> " + targetVol.volumeGuid);
        }
        return true;
    }

    // 4. Full Case B diskpart script execution (clears VDS/MountMgr hidden/nodefaultdriveletter/readonly/offline & assigns letter)
    if (targetVol.diskNumber != DWORD(-1) && targetVol.partNumber != DWORD(-1) && targetVol.partNumber > 0) {
        if (logCb) {
            logCb(std::wstring(L"Running diskpart Case-B recovery for ") + targetLetter +
                  L": (Disk " + std::to_wstring(targetVol.diskNumber) +
                  L", Partition " + std::to_wstring(targetVol.partNumber) + L")...");
        }

        std::string dp = "automount enable\r\n";
        if (!occupantGuid.empty() && occupantDisk != DWORD(-1) && occupantPart != DWORD(-1) && occupantPart > 0) {
            dp += "select disk " + std::to_string(occupantDisk) + "\r\n";
            dp += "select partition " + std::to_string(occupantPart) + "\r\n";
            dp += std::string("remove letter=") + static_cast<char>(targetLetter) + " noerr\r\n";
            if (!occupantIsSystemOrRec) {
                wchar_t fb = PickSafeFallbackLetter(reservedLetters);
                if (fb != 0) {
                    dp += std::string("assign letter=") + static_cast<char>(fb) + " noerr\r\n";
                }
            }
        }

        dp += "select disk " + std::to_string(targetVol.diskNumber) + "\r\n";
        dp += "online disk noerr\r\n";
        dp += "attributes disk clear readonly noerr\r\n";
        dp += "select partition " + std::to_string(targetVol.partNumber) + "\r\n";
        dp += "online volume noerr\r\n";
        dp += "attributes volume clear readonly noerr\r\n";
        dp += "attributes volume clear hidden noerr\r\n";
        dp += "attributes volume clear nodefaultdriveletter noerr\r\n";
        dp += std::string("assign letter=") + static_cast<char>(targetLetter) + " noerr\r\n";

        RunDiskpartScript(dp, logCb);
        SetVolumeMountPointW(targetRoot, targetVol.volumeGuid.c_str());
    }

    bool finalOk = isProperlyMountedNow();
    if (logCb) {
        logCb(finalOk
              ? (std::wstring(L"Successfully restored ") + targetLetter + L": via diskpart + MountMgr!")
              : (std::wstring(L"WARNING: Failed to verify ") + targetLetter + L": after recovery attempts."));
    }
    return finalOk;
}

} // namespace

int RunPeFixDrivesMode() {
    EnsureDirectoryRecursive(L"X:\\Windows\\Temp");
    std::wstring xLogPath = L"X:\\Windows\\Temp\\OneTimePE_DriveFix.log";

    auto drvLog = [&](const std::wstring& msg) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t ts[40];
        swprintf_s(ts, L"[%04d-%02d-%02d %02d:%02d:%02d] ",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::string u8 = WideToUtf8(std::wstring(ts) + msg + L"\r\n");
        HANDLE h = CreateFileW(xLogPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            WriteFile(h, u8.data(), static_cast<DWORD>(u8.size()), &wr, nullptr);
            CloseHandle(h);
        }
    };

    drvLog(L"=== OneTimePE WinPE Drive-Letter Auto-Recovery Engine Started ===");
    EnablePeAutoMount(drvLog);

    wchar_t targetOsLetter = L'C';
    std::vector<RecordedDriveTarget> recordedTargets;
    std::vector<PePartitionLayoutEntry> layouts;
    std::vector<PeVolumeEntry> vols;

    // Wait up to ~12 seconds if internal storage PnP (NVMe/VMD/SATA) is still enumerating at early boot
    for (int waitStep = 0; waitStep < 12; ++waitStep) {
        ClearInternalDisksOfflineAndReadOnly(waitStep == 0 ? drvLog : LogCallback{});
        layouts = ScanInternalPartitionLayouts();
        vols    = EnumeratePeVolumes(layouts, targetOsLetter, recordedTargets);
        if (!vols.empty() || !layouts.empty()) {
            bool anyOsOrMap = false;
            for (const auto& v : vols) {
                if (v.isWindowsOs || v.hasDriveMapIni) { anyOsOrMap = true; break; }
            }
            if (anyOsOrMap || waitStep >= 4) break;
        }
        Sleep(1000);
    }

    // If NO Windows OS volume was visible yet because GPT HIDDEN (0x4000000000000000) or MBR 0x17 kept NTFS unmounted,
    // probe hidden Basic Data partitions (>= 12 GB), keeping unhidden ONLY if confirmed to be a real Windows OS partition!
    bool foundAnyOsVol = false;
    for (const auto& v : vols) {
        if (v.isWindowsOs || v.hasDriveMapIni) { foundAnyOsVol = true; break; }
    }
    if (!foundAnyOsVol) {
        for (const auto& le : layouts) {
            if (le.isSystemOrRec || le.partLength < (12LL * 1024LL * 1024LL * 1024LL)) continue;
            const ULONGLONG hideBits = GPT_BASIC_DATA_ATTRIBUTE_HIDDEN |
                                       GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER |
                                       GPT_BASIC_DATA_ATTRIBUTE_READ_ONLY;
            bool isHiddenCandidate = (le.isGpt && (le.gptAttrs & hideBits) != 0) ||
                                     (!le.isGpt && le.mbrType == 0x17);
            if (!isHiddenCandidate) continue;

            drvLog(L"Probing hidden internal Basic Data partition (Disk " +
                   std::to_wstring(le.diskNumber) + L", Partition " + std::to_wstring(le.partNumber) + L")...");

            std::string probeDp =
                "automount enable\r\n"
                "select disk " + std::to_string(le.diskNumber) + "\r\n"
                "online disk noerr\r\n"
                "attributes disk clear readonly noerr\r\n"
                "select partition " + std::to_string(le.partNumber) + "\r\n";
            if (!le.isGpt && le.mbrType == 0x17) {
                probeDp += "set id=07 override noerr\r\n";
            }
            probeDp +=
                "online volume noerr\r\n"
                "attributes volume clear readonly noerr\r\n"
                "attributes volume clear hidden noerr\r\n"
                "attributes volume clear nodefaultdriveletter noerr\r\n";
            RunDiskpartScript(probeDp, drvLog);
            Sleep(500);

            vols = EnumeratePeVolumes(layouts, targetOsLetter, recordedTargets);
            bool confirmedOs = false;
            for (const auto& v : vols) {
                if (v.diskNumber == le.diskNumber && v.startOffset == le.startOffset &&
                    (v.isWindowsOs || v.hasDriveMapIni)) {
                    confirmedOs   = true;
                    foundAnyOsVol = true;
                    break;
                }
            }
            if (confirmedOs) {
                drvLog(L"Confirmed hidden partition IS the Windows OS volume!");
                break;
            } else {
                // Re-hide OEM recovery partition (e.g. SAMSUNG_REC2) so it remains untouched
                drvLog(L"Partition is not Windows OS (likely OEM recovery) — restoring hidden attributes.");
                std::string rehideDp =
                    "select disk " + std::to_string(le.diskNumber) + "\r\n"
                    "select partition " + std::to_string(le.partNumber) + "\r\n"
                    "remove noerr\r\n";
                if (le.isGpt) {
                    if (le.gptAttrs & GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER) {
                        rehideDp += "attributes volume set nodefaultdriveletter noerr\r\n";
                    }
                    if (le.gptAttrs & GPT_BASIC_DATA_ATTRIBUTE_HIDDEN) {
                        rehideDp += "attributes volume set hidden noerr\r\n";
                    }
                } else if (le.mbrType == 0x17) {
                    rehideDp += "set id=17 override noerr\r\n";
                }
                RunDiskpartScript(rehideDp, nullptr);
            }
        }
        vols = EnumeratePeVolumes(layouts, targetOsLetter, recordedTargets);
    }

    // Match helper for recorded targets
    auto matchRecordedVolume = [&](const RecordedDriveTarget& rt) -> const PeVolumeEntry* {
        if (!rt.volumeGuid.empty()) {
            for (const auto& v : vols) {
                if (v.volumeGuid == rt.volumeGuid) return &v;
            }
        }
        if (rt.startOffset >= 0 && rt.extentLen > 0) {
            for (const auto& v : vols) {
                if (v.isInternal && v.startOffset == rt.startOffset && v.extentLen == rt.extentLen) {
                    return &v;
                }
            }
        }
        if (rt.serial != 0) {
            for (const auto& v : vols) {
                if (v.isInternal && !v.isSystemOrRec && v.serial == rt.serial) return &v;
            }
        }
        return nullptr;
    };

    // Select Primary OS Volume
    const PeVolumeEntry* primaryOsVol = nullptr;
    for (const auto& rt : recordedTargets) {
        if (rt.isOs) {
            primaryOsVol   = matchRecordedVolume(rt);
            targetOsLetter = rt.letter;
            break;
        }
    }
    if (!primaryOsVol) {
        ULONGLONG bestScore = 0;
        for (const auto& v : vols) {
            if (!v.isWindowsOs && !v.hasDriveMapIni) continue;
            ULONGLONG score = (v.hasDriveMapIni ? (1ULL << 62) : 0ULL) + (v.osHiveTime >> 4);
            if (!primaryOsVol || score > bestScore) {
                primaryOsVol = &v;
                bestScore    = score;
            }
        }
    }

    if (!primaryOsVol) {
        drvLog(L"No internal Windows OS NTFS partition found (may be BitLocker locked or raw disk).");
        return 1;
    }

    drvLog(L"Target Windows OS Volume selected: " + primaryOsVol->volumeGuid +
           L" -> Letter " + std::wstring(1, targetOsLetter) + L":");

    std::set<wchar_t> reservedLetters;
    reservedLetters.insert(targetOsLetter);
    reservedLetters.insert(L'X');
    reservedLetters.insert(L'Y'); // Protect Custom PE Y: mount
    for (const auto& rt : recordedTargets) {
        reservedLetters.insert(rt.letter);
    }

    std::vector<wchar_t> changedLetters;
    bool osOk = ApplySingleDriveLetterRestore(*primaryOsVol, targetOsLetter, primaryOsVol->isWindowsOs,
                                              vols, reservedLetters, drvLog);
    if (osOk) changedLetters.push_back(targetOsLetter);

    // Also restore any additional recorded internal fixed drives (D:, E:, ...), respecting Custom PE's Y: if mounted
    DWORD curDriveMask = GetLogicalDrives();
    for (const auto& rt : recordedTargets) {
        if (rt.isOs || rt.letter == targetOsLetter || rt.letter == L'X') continue;
        if (rt.letter == L'Y' && (curDriveMask & (1u << (L'Y' - L'A'))) != 0) {
            drvLog(L"Skipping Y: data restore because Custom PE already uses Y: for external tools.");
            continue;
        }
        const PeVolumeEntry* dv = matchRecordedVolume(rt);
        if (dv && dv->volumeGuid != primaryOsVol->volumeGuid && !dv->isSystemOrRec) {
            if (ApplySingleDriveLetterRestore(*dv, rt.letter, false, vols, reservedLetters, drvLog)) {
                changedLetters.push_back(rt.letter);
            }
        }
    }

    if (!changedLetters.empty()) {
        NotifyShellDriveChanges(changedLetters);
    }

    // Post-boot 12-second anti-hijack watchdog: if a late PECMD.INI script detaches or steals C:, re-secure it immediately!
    std::wstring savedOsGuid = primaryOsVol->volumeGuid;
    PeVolumeEntry savedOsSnapshot = *primaryOsVol;
    wchar_t osRoot[4] = { targetOsLetter, L':', L'\\', L'\0' };

    for (int watch = 0; watch < 6; ++watch) {
        Sleep(2000);
        wchar_t checkGuid[128]{};
        bool stillMounted = GetVolumeNameForVolumeMountPointW(osRoot, checkGuid, _countof(checkGuid)) != FALSE;
        std::wstring lowCheck = ToLower(checkGuid);
        if (!lowCheck.empty() && lowCheck.back() != L'\\') lowCheck += L'\\';
        if (!stillMounted || lowCheck != savedOsGuid) {
            drvLog(L"Watchdog detected late PE script altered " + std::wstring(1, targetOsLetter) +
                   L": — re-applying Windows OS drive letter!");
            auto freshVols = EnumeratePeVolumes(layouts, targetOsLetter, recordedTargets);
            const PeVolumeEntry* targetPtr = &savedOsSnapshot;
            for (const auto& fv : freshVols) {
                if (fv.volumeGuid == savedOsGuid) { targetPtr = &fv; break; }
            }
            if (ApplySingleDriveLetterRestore(*targetPtr, targetOsLetter, targetPtr->isWindowsOs,
                                              freshVols, reservedLetters, drvLog)) {
                NotifyShellDriveChanges({ targetOsLetter });
            }
        }
    }

    drvLog(L"=== OneTimePE WinPE Drive-Letter Auto-Recovery Completed ===");

    // Mirror log to C:\Windows\Temp\OneTimePE_DriveFix.log so user can review after rebooting to Windows
    std::wstring hostTempDir = std::wstring(1, targetOsLetter) + L":\\Windows\\Temp";
    if (DirExists(hostTempDir)) {
        CopyFileW(xLogPath.c_str(), JoinPath(hostTempDir, L"OneTimePE_DriveFix.log").c_str(), FALSE);
    }

    return osOk ? 0 : 1;
}

// ============================================================================
// AnySupport (ManualLauncher.exe) Auto-Connect Engine inside WinPE
// (Topmost Window Elevation + Caret Focus on Code Input + Hardware Keyboard Type & Enter)
// ============================================================================
int RunPeRemoteSupportMode(const std::wstring& code, const std::wstring& exeName) {
    if (code.empty()) return 0;

    HANDLE hRemMutex = CreateMutexW(nullptr, FALSE, L"Local\\OneTimePE_RemoteSupport_Active");
    if (hRemMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hRemMutex);
        return 0;
    }

    auto logRemote = [](const std::wstring& msg) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t ts[40]{};
        swprintf_s(ts, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        std::string line = WideToUtf8(std::wstring(ts) + msg) + "\r\n";

        wchar_t winDir[MAX_PATH]{};
        GetWindowsDirectoryW(winDir, MAX_PATH);
        std::wstring l1 = JoinPath(winDir, L"Temp\\OneTimePE_RemoteSupport.log");
        std::wstring l2 = L"C:\\Windows\\Temp\\OneTimePE_RemoteSupport.log";
        AppendAsciiFile(l1, line);
        if (_wcsicmp(l1.c_str(), l2.c_str()) != 0) {
            AppendAsciiFile(l2, line);
        }

        // Mirror log to all connected host drives (PEBoot\OneTimePE_RemoteSupport.log)
        // so it persists on physical disk even after WinPE reboot
        for (wchar_t d = L'C'; d <= L'Z'; ++d) {
            if (d == L'X') continue;
            std::wstring root = std::wstring(1, d) + L":\\";
            UINT dt = GetDriveTypeW(root.c_str());
            if (dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE) {
                std::wstring pebootDir = std::wstring(1, d) + L":\\PEBoot";
                if (GetFileAttributesW(pebootDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    AppendAsciiFile(JoinPath(pebootDir, L"OneTimePE_RemoteSupport.log"), line);
                }
                AppendAsciiFile(std::wstring(1, d) + L":\\OneTimePE_RemoteSupport.log", line);
            }
        }
    };

    std::wstring targetExe = exeName.empty() ? L"ManualLauncher.exe" : exeName;
    logRemote(L"============================================================");
    logRemote(L"애니서포트(원격지원) 정밀 창 선택 & 키보드 자동 입력 엔진 시작");
    logRemote(L"• 인증번호: " + code);
    logRemote(L"• 대상 실행 파일명: " + targetExe);
    logRemote(L"============================================================");

    DWORD selfPid = GetCurrentProcessId();

    // Helper: Collect target PIDs (ManualLauncher.exe and any child processes it spawned)
    auto findTargetPids = [&](const std::wstring& name) -> std::vector<DWORD> {
        std::vector<DWORD> pids;
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) return pids;
        struct ProcNode { DWORD pid; DWORD ppid; std::wstring exe; };
        std::vector<ProcNode> all;
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (pe.th32ProcessID == selfPid) continue;
                all.push_back({ pe.th32ProcessID, pe.th32ParentProcessID, ToLower(pe.szExeFile) });
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);

        std::wstring lowTarget = ToLower(name);
        for (const auto& p : all) {
            if (p.exe == lowTarget ||
                p.exe == L"manuallauncher.exe" ||
                p.exe.find(L"anysupport") != std::wstring::npos) {
                pids.push_back(p.pid);
            }
        }
        // Include direct child processes of matched launcher PIDs
        size_t baseCount = pids.size();
        for (size_t i = 0; i < baseCount; ++i) {
            DWORD parentPid = pids[i];
            for (const auto& p : all) {
                if (p.ppid == parentPid &&
                    p.exe != L"cmd.exe" && p.exe != L"conhost.exe" &&
                    std::find(pids.begin(), pids.end(), p.pid) == pids.end()) {
                    pids.push_back(p.pid);
                }
            }
        }
        return pids;
    };

    // Helper: Find and score candidate top-level windows to select the TRUE active dialog
    struct ScoredWindow {
        HWND hWnd = nullptr;
        int score = 0;
        std::wstring title;
        std::wstring cls;
    };

    auto findBestWindow = [&](const std::vector<DWORD>& targetPids) -> HWND {
        std::vector<ScoredWindow> candidates;
        struct EnumCtx {
            std::vector<ScoredWindow>* list;
            const std::vector<DWORD>* pids;
        } ctx{ &candidates, &targetPids };

        EnumWindows([](HWND hWnd, LPARAM lp) -> BOOL {
            auto* pCtx = reinterpret_cast<EnumCtx*>(lp);
            auto* list = pCtx->list;
            if (!IsWindowVisible(hWnd) || IsIconic(hWnd)) return TRUE;

            RECT rc{};
            GetWindowRect(hWnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            if (w < 180 || h < 100) return TRUE;

            wchar_t clsBuf[128]{};
            GetClassNameW(hWnd, clsBuf, _countof(clsBuf));
            std::wstring cls = ToLower(clsBuf);
            if (cls == L"shell_traywnd" || cls == L"progman" || cls == L"workerw" ||
                cls == L"consolewindowclass") {
                return TRUE;
            }

            wchar_t titleBuf[256]{};
            GetWindowTextW(hWnd, titleBuf, _countof(titleBuf));
            std::wstring title = ToLower(titleBuf);

            DWORD pid = 0;
            GetWindowThreadProcessId(hWnd, &pid);

            // If hWnd has an active popup dialog (modal or child dialog), consider the popup directly!
            HWND hPopup = GetLastActivePopup(hWnd);
            if (hPopup && hPopup != hWnd && IsWindowVisible(hPopup) && !IsIconic(hPopup)) {
                hWnd = hPopup;
                GetWindowTextW(hWnd, titleBuf, _countof(titleBuf));
                title = ToLower(titleBuf);
                GetClassNameW(hWnd, clsBuf, _countof(clsBuf));
                cls = ToLower(clsBuf);
                GetWindowRect(hWnd, &rc);
                w = rc.right - rc.left;
                h = rc.bottom - rc.top;
            }

            int score = 0;
            if (title.find(L"애니서포트") != std::wstring::npos || title.find(L"anysupport") != std::wstring::npos) score += 150;
            if (title.find(L"접속번호") != std::wstring::npos || title.find(L"인증번호") != std::wstring::npos) score += 120;
            if (title.find(L"원격지원") != std::wstring::npos || title.find(L"원격") != std::wstring::npos) score += 90;

            if (pCtx->pids) {
                for (DWORD p : *(pCtx->pids)) {
                    if (p != 0 && p == pid) {
                        score += 120;
                        break;
                    }
                }
            }

            // Check if process matches
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProc) {
                wchar_t imgPath[MAX_PATH * 2]{};
                DWORD imgSize = _countof(imgPath);
                if (QueryFullProcessImageNameW(hProc, 0, imgPath, &imgSize)) {
                    std::wstring fn = ToLower(FileNameFromPath(imgPath));
                    if (fn == L"manuallauncher.exe" || fn.find(L"anysupport") != std::wstring::npos) {
                        score += 100;
                    }
                }
                CloseHandle(hProc);
            }

            // Count visible child controls
            int childCount = 0;
            EnumChildWindows(hWnd, [](HWND, LPARAM lpCount) -> BOOL {
                (*reinterpret_cast<int*>(lpCount))++;
                return TRUE;
            }, reinterpret_cast<LPARAM>(&childCount));
            score += std::min(childCount * 5, 50);

            if (score > 0) {
                list->push_back({ hWnd, score, titleBuf, clsBuf });
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

        if (candidates.empty()) return nullptr;
        std::sort(candidates.begin(), candidates.end(), [](const ScoredWindow& a, const ScoredWindow& b) {
            return a.score > b.score;
        });
        return candidates[0].hWnd;
    };

    // 1. Wait for AnySupport window to appear (up to 90 seconds)
    HWND hTargetWnd = nullptr;
    logRemote(L"커스텀 PE에서 실행된 애니서포트 창 감시 중 (최대 90초 대기)...");

    for (int t = 0; t < 180; ++t) {
        std::vector<DWORD> pids = findTargetPids(targetExe);
        HWND hCandidate = findBestWindow(pids);
        if (hCandidate != nullptr) {
            hTargetWnd = hCandidate;
            break;
        }
        Sleep(500);
    }

    if (!hTargetWnd) {
        logRemote(L"경고: 90초 대기 시간 내에 애니서포트 창이 감지되지 않아 종료합니다.");
        if (hRemMutex) CloseHandle(hRemMutex);
        return 1;
    }

    wchar_t foundTitle[256]{};
    GetWindowTextW(hTargetWnd, foundTitle, _countof(foundTitle));
    wchar_t foundCls[128]{};
    GetClassNameW(hTargetWnd, foundCls, _countof(foundCls));
    logRemote(L"★ 애니서포트 창 감지 완료! 제목=[" + std::wstring(foundTitle) +
              L"], 클래스=[" + std::wstring(foundCls) +
              L"], HWND=" + std::to_wstring(reinterpret_cast<uintptr_t>(hTargetWnd)));

    // 2. Wait 1.5s for other PE startup windows (PECMD/CMD/PENetwork) to finish popping up & UI controls to initialize
    Sleep(1500);

    // 3. Hardware Keyboard Simulation Helpers (SendInput with Virtual Key + Scan Code)
    auto sendKeyTap = [](WORD vk, DWORD holdMs = 35, DWORD afterMs = 45) {
        WORD scan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = vk;
        in[0].ki.wScan = scan;

        in[1].type = INPUT_KEYBOARD;
        in[1].ki.wVk = vk;
        in[1].ki.wScan = scan;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;

        SendInput(1, &in[0], sizeof(INPUT));
        Sleep(holdMs);
        SendInput(1, &in[1], sizeof(INPUT));
        Sleep(afterMs);
    };

    auto sendCtrlKey = [](WORD vk) {
        WORD scanCtrl = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
        WORD scanKey  = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        INPUT dn[2]{};
        dn[0].type = INPUT_KEYBOARD; dn[0].ki.wVk = VK_CONTROL; dn[0].ki.wScan = scanCtrl;
        dn[1].type = INPUT_KEYBOARD; dn[1].ki.wVk = vk;         dn[1].ki.wScan = scanKey;
        SendInput(2, dn, sizeof(INPUT));
        Sleep(35);

        INPUT up[2]{};
        up[0].type = INPUT_KEYBOARD; up[0].ki.wVk = vk;         up[0].ki.wScan = scanKey;  up[0].ki.dwFlags = KEYEVENTF_KEYUP;
        up[1].type = INPUT_KEYBOARD; up[1].ki.wVk = VK_CONTROL; up[1].ki.wScan = scanCtrl; up[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, up, sizeof(INPUT));
        Sleep(45);
    };

    auto sendUnicodeChar = [](wchar_t ch) {
        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wScan = ch;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;

        in[1].type = INPUT_KEYBOARD;
        in[1].ki.wScan = ch;
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

        SendInput(1, &in[0], sizeof(INPUT));
        Sleep(30);
        SendInput(1, &in[1], sizeof(INPUT));
        Sleep(45);
    };

    auto typeStringViaKeyboard = [&](const std::wstring& str) {
        for (wchar_t ch : str) {
            if (ch >= L'0' && ch <= L'9') {
                sendKeyTap(static_cast<WORD>(L'0' + (ch - L'0')), 35, 45);
            } else if (ch >= L'a' && ch <= L'z') {
                sendKeyTap(static_cast<WORD>(L'A' + (ch - L'a')), 35, 45);
            } else if (ch >= L'A' && ch <= L'Z') {
                sendKeyTap(static_cast<WORD>(L'A' + (ch - L'A')), 35, 45);
            } else {
                sendUnicodeChar(ch);
            }
        }
    };

    // 4. Forceful Window Selection Helper:
    //    Demotes any obstructing top windows to HWND_BOTTOM and elevates hTargetWnd to HWND_TOPMOST
    //    WITHOUT sending random WM_NEXTDLGCTL or SetFocus calls that disrupt input controls!
    auto forceSelectWindow = [&](HWND hWndTarget) {
        if (!IsWindow(hWndTarget)) return;

        // A. Demote any other window currently sitting in the foreground or topmost
        HWND hFg = GetForegroundWindow();
        if (hFg && hFg != hWndTarget) {
            wchar_t fgTitle[128]{};
            wchar_t fgCls[128]{};
            GetWindowTextW(hFg, fgTitle, _countof(fgTitle));
            GetClassNameW(hFg, fgCls, _countof(fgCls));
            if (_wcsicmp(fgCls, L"Shell_TrayWnd") != 0 && _wcsicmp(fgCls, L"Progman") != 0) {
                logRemote(L"상위 가로막음 창 감지 -> 후면(HWND_BOTTOM) 이동: [" +
                          std::wstring(fgTitle) + L" / " + std::wstring(fgCls) + L"]");
                SetWindowPos(hFg, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                SetWindowPos(hFg, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }

        // Demote all other visible topmost windows
        EnumWindows([](HWND hTop, LPARAM lp) -> BOOL {
            HWND hTarget = reinterpret_cast<HWND>(lp);
            if (hTop == hTarget || !IsWindowVisible(hTop)) return TRUE;
            LONG exStyle = GetWindowLongW(hTop, GWL_EXSTYLE);
            if (exStyle & WS_EX_TOPMOST) {
                wchar_t cBuf[128]{};
                GetClassNameW(hTop, cBuf, _countof(cBuf));
                if (_wcsicmp(cBuf, L"Shell_TrayWnd") != 0) {
                    SetWindowPos(hTop, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    SetWindowPos(hTop, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(hWndTarget));

        SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, nullptr, SPIF_SENDCHANGE);
        AllowSetForegroundWindow(ASFW_ANY);

        if (IsIconic(hWndTarget)) {
            ShowWindow(hWndTarget, SW_RESTORE);
        } else {
            ShowWindow(hWndTarget, SW_SHOW);
        }
        SetWindowPos(hWndTarget, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

        DWORD curThreadId = GetCurrentThreadId();
        DWORD fgThreadId  = hFg ? GetWindowThreadProcessId(hFg, nullptr) : 0;
        DWORD tgtThreadId = GetWindowThreadProcessId(hWndTarget, nullptr);

        bool attachedFg  = false;
        bool attachedTgt = false;
        if (fgThreadId != 0 && fgThreadId != curThreadId) {
            attachedFg = (AttachThreadInput(curThreadId, fgThreadId, TRUE) != FALSE);
        }
        if (tgtThreadId != 0 && tgtThreadId != curThreadId && tgtThreadId != fgThreadId) {
            attachedTgt = (AttachThreadInput(curThreadId, tgtThreadId, TRUE) != FALSE);
        }

        // Alt key state unlock for SetForegroundWindow
        keybd_event(VK_MENU, 0, KEYEVENTF_EXTENDEDKEY, 0);
        keybd_event(VK_MENU, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);

        BringWindowToTop(hWndTarget);
        SetForegroundWindow(hWndTarget);
        SetActiveWindow(hWndTarget);
        SendMessageW(hWndTarget, WM_ACTIVATE, WA_ACTIVE, 0);

        if (attachedTgt) AttachThreadInput(curThreadId, tgtThreadId, FALSE);
        if (attachedFg)  AttachThreadInput(curThreadId, fgThreadId, FALSE);
        Sleep(150);
    };

    // 5. Structure to hold all enumerated child controls
    struct ChildCtrl {
        HWND hWnd = nullptr;
        std::wstring cls;
        std::wstring text;
        int id = 0;
        RECT rc{};
        int w = 0;
        int h = 0;
    };

    for (int pass = 1; pass <= 3; ++pass) {
        if (!IsWindow(hTargetWnd) || !IsWindowVisible(hTargetWnd)) {
            logRemote(L"애니서포트 창이 닫히거나 연결 화면으로 전환됨 (Pass " + std::to_wstring(pass) + L")");
            break;
        }

        logRemote(L"--- 키보드 입력 및 연결 시도 (Pass " + std::to_wstring(pass) + L"/3) ---");

        // Step A: Elevate window to topmost foreground
        forceSelectWindow(hTargetWnd);
        logRemote(L"창 최상위 선택 완료 (Foreground=" +
                  std::to_wstring(reinterpret_cast<uintptr_t>(GetForegroundWindow())) + L")");

        // Enumerate and catalog all child controls
        std::vector<ChildCtrl> children;
        EnumChildWindows(hTargetWnd, [](HWND hChild, LPARAM lParam) -> BOOL {
            auto* vec = reinterpret_cast<std::vector<ChildCtrl>*>(lParam);
            wchar_t clsBuf[128]{};
            GetClassNameW(hChild, clsBuf, _countof(clsBuf));
            wchar_t txtBuf[256]{};
            GetWindowTextW(hChild, txtBuf, _countof(txtBuf));
            RECT r{};
            GetWindowRect(hChild, &r);
            vec->push_back({ hChild, ToLower(clsBuf), txtBuf, GetDlgCtrlID(hChild), r, r.right - r.left, r.bottom - r.top });
            return TRUE;
        }, reinterpret_cast<LPARAM>(&children));

        HWND hTargetEdit = nullptr;
        HWND hBtnConnect = nullptr;

        for (const auto& c : children) {
            if (pass == 1) {
                logRemote(L" • 자식 컨트롤: cls=[" + c.cls + L"], id=" + std::to_wstring(c.id) +
                          L", w=" + std::to_wstring(c.w) + L", h=" + std::to_wstring(c.h) +
                          L", text=[" + c.text + L"]");
            }
            if (!IsWindowVisible(c.hWnd) || !IsWindowEnabled(c.hWnd)) continue;

            // Search for single-line editable input control
            if (c.cls.find(L"edit") != std::wstring::npos) {
                LONG style = GetWindowLongW(c.hWnd, GWL_STYLE);
                if (!(style & ES_READONLY) && c.h >= 14 && c.h <= 60 && c.w >= 30) {
                    // Prefer currently focused control or first valid edit
                    if (!hTargetEdit || GetFocus() == c.hWnd) {
                        hTargetEdit = c.hWnd;
                    }
                }
            } else if (c.cls.find(L"button") != std::wstring::npos) {
                LONG bStyle = GetWindowLongW(c.hWnd, GWL_STYLE) & BS_TYPEMASK;
                if (bStyle != BS_CHECKBOX && bStyle != BS_AUTOCHECKBOX &&
                    bStyle != BS_RADIOBUTTON && bStyle != BS_AUTORADIOBUTTON) {
                    std::wstring lowT = ToLower(c.text);
                    int bId = c.id;
                    // Explicitly ignore cancel/close/help buttons
                    if (bId != IDCANCEL && bId != 2 &&
                        lowT.find(L"취소") == std::wstring::npos &&
                        lowT.find(L"닫기") == std::wstring::npos &&
                        lowT.find(L"cancel") == std::wstring::npos &&
                        lowT.find(L"close") == std::wstring::npos &&
                        lowT.find(L"도움") == std::wstring::npos) {
                        if (lowT.find(L"연결") != std::wstring::npos ||
                            lowT.find(L"접속") != std::wstring::npos ||
                            lowT.find(L"확인") != std::wstring::npos ||
                            lowT.find(L"시작") != std::wstring::npos ||
                            lowT.find(L"connect") != std::wstring::npos ||
                            lowT.find(L"ok") != std::wstring::npos ||
                            lowT.find(L"start") != std::wstring::npos ||
                            bId == IDOK || bId == 1) {
                            hBtnConnect = c.hWnd;
                        }
                    }
                }
            }
        }

        // Step B: Focus the passcode input field cleanly
        if (hTargetEdit && IsWindow(hTargetEdit)) {
            logRemote(L"인증번호 입력 컨트롤 발견 (ID: " + std::to_wstring(GetDlgCtrlID(hTargetEdit)) + L") -> 직접 포커스 및 캐럿 활성화");
            SetFocus(hTargetEdit);

            // Simulate left-click directly inside the center of the edit box to guarantee cursor placement
            RECT rcEdit{};
            GetClientRect(hTargetEdit, &rcEdit);
            int cx = (rcEdit.right - rcEdit.left) / 2;
            int cy = (rcEdit.bottom - rcEdit.top) / 2;
            SendMessageW(hTargetEdit, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
            SendMessageW(hTargetEdit, WM_LBUTTONUP, 0, MAKELPARAM(cx, cy));
            Sleep(80);

            // Clear any existing text
            SendMessageW(hTargetEdit, EM_SETSEL, 0, -1);
            SendMessageW(hTargetEdit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L""));
        } else {
            logRemote(L"표준 Edit 컨트롤 미발견 (커스텀 스킨/DirectUI 창) -> 창 기본 포커스 유지");
        }

        // Keyboard clear: Ctrl+A -> Backspace
        sendCtrlKey('A');
        sendKeyTap(VK_BACK, 25, 30);
        sendKeyTap(VK_DELETE, 25, 30);
        Sleep(50);

        // Step C: Input the authentication code
        // 1) Direct WM_SETTEXT if edit control is present
        bool textSetOk = false;
        if (hTargetEdit && IsWindow(hTargetEdit)) {
            SendMessageW(hTargetEdit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(code.c_str()));
            SendMessageW(hTargetWnd, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hTargetEdit), EN_CHANGE), reinterpret_cast<LPARAM>(hTargetEdit));
            SendMessageW(hTargetWnd, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hTargetEdit), EN_UPDATE), reinterpret_cast<LPARAM>(hTargetEdit));

            wchar_t vBuf[128]{};
            GetWindowTextW(hTargetEdit, vBuf, _countof(vBuf));
            if (wcscmp(vBuf, code.c_str()) == 0) {
                textSetOk = true;
                logRemote(L"인증번호 WM_SETTEXT 주입 검증 완료: [" + code + L"]");
            }
        }

        // 2) Hardware SendInput typing
        logRemote(L"키보드(SendInput)로 인증번호 [" + code + L"] 타이핑 전송");
        typeStringViaKeyboard(code);
        Sleep(150);

        // 3) Clipboard paste fallback if input is still empty
        if (hTargetEdit && IsWindow(hTargetEdit)) {
            wchar_t vBuf[128]{};
            GetWindowTextW(hTargetEdit, vBuf, _countof(vBuf));
            if (wcslen(vBuf) == 0 && !textSetOk) {
                logRemote(L"입력칸이 비어 있어 클립보드 붙여넣기(Ctrl+V) 보조 실행");
                if (OpenClipboard(nullptr)) {
                    EmptyClipboard();
                    size_t bytes = (code.size() + 1) * sizeof(wchar_t);
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                    if (hMem) {
                        void* p = GlobalLock(hMem);
                        if (p) {
                            memcpy(p, code.c_str(), bytes);
                            GlobalUnlock(hMem);
                            SetClipboardData(CF_UNICODETEXT, hMem);
                        }
                    }
                    CloseClipboard();
                }
                sendCtrlKey('V');
                Sleep(100);
            }
        }

        // Step D: Trigger Connect!
        // 1) First and foremost: Press Enter (VK_RETURN) directly in the active input box
        logRemote(L"1차 연결 실행: 입력창에서 Enter(VK_RETURN) 키 전송");
        sendKeyTap(VK_RETURN, 50, 80);
        PostMessageW(hTargetWnd, WM_KEYDOWN, VK_RETURN, 0);
        PostMessageW(hTargetWnd, WM_KEYUP, VK_RETURN, 0);
        if (hTargetEdit && IsWindow(hTargetEdit)) {
            PostMessageW(hTargetEdit, WM_KEYDOWN, VK_RETURN, 0);
            PostMessageW(hTargetEdit, WM_KEYUP, VK_RETURN, 0);
        }
        Sleep(700);

        // 2) If a verified Connect button was identified and window is still open, click it directly
        if (IsWindow(hTargetWnd) && IsWindowVisible(hTargetWnd) && hBtnConnect && IsWindow(hBtnConnect)) {
            wchar_t btnTxt[64]{};
            GetWindowTextW(hBtnConnect, btnTxt, _countof(btnTxt));
            logRemote(L"2차 연결 실행: 검증된 연결 버튼 [" + std::wstring(btnTxt) + L" (ID: " +
                      std::to_wstring(GetDlgCtrlID(hBtnConnect)) + L")] 클릭 메시지 전송");
            SendMessageW(hBtnConnect, BM_CLICK, 0, 0);
            PostMessageW(hTargetWnd, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hBtnConnect), BN_CLICKED), reinterpret_cast<LPARAM>(hBtnConnect));

            RECT rcBtn{};
            GetClientRect(hBtnConnect, &rcBtn);
            int bx = (rcBtn.right - rcBtn.left) / 2;
            int by = (rcBtn.bottom - rcBtn.top) / 2;
            SendMessageW(hBtnConnect, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(bx, by));
            SendMessageW(hBtnConnect, WM_LBUTTONUP, 0, MAKELPARAM(bx, by));
        }

        // Wait 2.5s and check connection progress
        Sleep(2500);
        if (!IsWindow(hTargetWnd) || !IsWindowVisible(hTargetWnd)) {
            logRemote(L"★ 애니서포트 연결 진행 확인됨 (입력창 닫힘/전환 완료)!");
            break;
        }
        if (hTargetEdit && (!IsWindow(hTargetEdit) || !IsWindowVisible(hTargetEdit) || !IsWindowEnabled(hTargetEdit))) {
            logRemote(L"★ 애니서포트 연결 진행 확인됨 (입력 컨트롤 비활성/전환 완료)!");
            break;
        }
    }

    // Restore normal Z-order so subsequent remote screen streaming windows work normally
    if (IsWindow(hTargetWnd)) {
        SetWindowPos(hTargetWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    logRemote(L"=== 애니서포트 원격지원 자동화 루틴 종료 ===");
    if (hRemMutex) CloseHandle(hRemMutex);
    return 0;
}

int RunPeBootInitMode(const std::wstring& appDir) {
    // Prevent duplicate concurrent execution across startnet.cmd + winpeshl.ini + PECMD.INI + Startup hooks
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"Local\\OneTimePE_BootInit_Active");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;
    }

    EnsureDirectoryRecursive(L"X:\\Windows\\Temp");
    std::wstring doneFlag = L"X:\\Windows\\Temp\\OneTimePE_BootInitDone.flg";
    if (GetFileAttributesW(doneFlag.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }
    WriteAsciiFile(doneFlag, "1\r\n");

    // 1. Always restore C: (and recorded internal drives) FIRST so host partitions are accessible!
    RunPeFixDrivesMode();

    // 2. Check if Auto-Network Sync and Remote Support were requested in OneTimePE_Bridge.ini
    bool shouldApplyNet = false;
    bool shouldRemoteSupport = false;
    std::wstring remoteCode;
    std::wstring remoteExe = L"ManualLauncher.exe";
    std::wstring foundBridgeDir;

    for (wchar_t drv = L'C'; drv <= L'Z'; ++drv) {
        if (drv == L'X') continue;
        std::wstring candDir = std::wstring(1, drv) + L":\\Windows\\Temp";
        std::wstring candIni = JoinPath(candDir, L"OneTimePE_Bridge.ini");
        if (GetFileAttributesW(candIni.c_str()) != INVALID_FILE_ATTRIBUTES) {
            int autoNetVal = static_cast<int>(GetPrivateProfileIntW(L"Bridge", L"AutoApplyNet", 0, candIni.c_str()));
            if (autoNetVal == 1) {
                shouldApplyNet = true;
            }
            int remVal = static_cast<int>(GetPrivateProfileIntW(L"RemoteSupport", L"Enable", 0, candIni.c_str()));
            if (remVal == 1) {
                wchar_t cBuf[128]{};
                GetPrivateProfileStringW(L"RemoteSupport", L"Code", L"", cBuf, _countof(cBuf), candIni.c_str());
                wchar_t eBuf[MAX_PATH]{};
                GetPrivateProfileStringW(L"RemoteSupport", L"ExeName", L"ManualLauncher.exe", eBuf, _countof(eBuf), candIni.c_str());
                remoteCode = Trim(cBuf);
                remoteExe  = Trim(eBuf);
                if (remoteExe.empty()) remoteExe = L"ManualLauncher.exe";
                if (!remoteCode.empty()) {
                    shouldRemoteSupport = true;
                }
            }
            foundBridgeDir = candDir;
            break;
        }
    }

    int rc = 0;
    if (shouldApplyNet) {
        rc = RunPeAutoNetMode(foundBridgeDir.empty() ? appDir : foundBridgeDir);
    }

    // 3. Launch AnySupport Auto-Connect as a persistent independent process (using latest OneTimePE_Runner.exe if available)
    if (shouldRemoteSupport && !remoteCode.empty()) {
        std::wstring runnerExe;
        if (!foundBridgeDir.empty()) {
            std::wstring candRunner = JoinPath(foundBridgeDir, L"OneTimePE_Runner.exe");
            if (GetFileAttributesW(candRunner.c_str()) != INVALID_FILE_ATTRIBUTES) {
                runnerExe = candRunner;
            }
        }
        if (runnerExe.empty()) {
            wchar_t selfBuf[MAX_PATH * 4]{};
            if (GetModuleFileNameW(nullptr, selfBuf, _countof(selfBuf)) > 0) {
                runnerExe = selfBuf;
            }
        }

        bool launchedChild = false;
        if (!runnerExe.empty()) {
            std::wstring cmdLine = L"\"" + runnerExe + L"\" --pe-remote-support " + remoteCode + L" " + remoteExe;
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(runnerExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                launchedChild = true;
            }
        }
        if (!launchedChild) {
            RunPeRemoteSupportMode(remoteCode, remoteExe);
        }
    }

    if (hMutex) CloseHandle(hMutex);
    return rc;
}

// ============================================================================
// Silent execution engine inside WinPE: "OneTimePE.exe --pe-apply-net"
// ============================================================================
int RunPeAutoNetMode(const std::wstring& appDir) {
    std::wstring resolvedDir = appDir;
    if (GetFileAttributesW(JoinPath(resolvedDir, L"NetConfig\\netconfig.ini").c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring bridgeIni = JoinPath(resolvedDir, L"OneTimePE_Bridge.ini");
        if (GetFileAttributesW(bridgeIni.c_str()) == INVALID_FILE_ATTRIBUTES) {
            for (wchar_t drv = L'C'; drv <= L'Z'; ++drv) {
                if (drv == L'X') continue;
                std::wstring cand = std::wstring(1, drv) + L":\\Windows\\Temp\\OneTimePE_Bridge.ini";
                if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    bridgeIni = cand;
                    break;
                }
            }
        }
        if (GetFileAttributesW(bridgeIni.c_str()) != INVALID_FILE_ATTRIBUTES) {
            wchar_t relBuf[MAX_PATH * 2]{};
            GetPrivateProfileStringW(L"Bridge", L"RelAppDir", L"", relBuf, _countof(relBuf), bridgeIni.c_str());
            std::wstring rel = Trim(relBuf);
            if (!rel.empty()) {
                bool foundTarget = false;
                for (int retry = 0; retry < 10 && !foundTarget; ++retry) {
                    if (retry > 0) Sleep(1000);
                    for (wchar_t drv = L'C'; drv <= L'Z'; ++drv) {
                        if (drv == L'X') continue;
                        std::wstring candidate = std::wstring(1, drv) + L":" +
                                                 (rel.front() == L'\\' ? rel : (L"\\" + rel));
                        if (GetFileAttributesW(JoinPath(candidate, L"NetConfig\\netconfig.ini").c_str()) != INVALID_FILE_ATTRIBUTES) {
                            resolvedDir = candidate;
                            foundTarget = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    std::wstring netDir  = JoinPath(resolvedDir, L"NetConfig");
    std::wstring drvDir  = JoinPath(netDir, L"Drivers");
    std::wstring wifiDir = JoinPath(netDir, L"WiFi");
    std::wstring cfgIni  = JoinPath(netDir, L"netconfig.ini");
    std::wstring logPath = JoinPath(netDir, L"pe_autonet.log");
    EnsureDirectoryRecursive(netDir);

    auto peLog = [&](const std::wstring& msg) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t ts[40];
        swprintf_s(ts, L"[%04d-%02d-%02d %02d:%02d:%02d] ",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::string u8 = WideToUtf8(std::wstring(ts) + msg + L"\r\n");

        for (const std::wstring& lp : { logPath, std::wstring(L"X:\\Windows\\Temp\\OneTimePE_Net.log") }) {
            HANDLE h = CreateFileW(lp.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                WriteFile(h, u8.data(), static_cast<DWORD>(u8.size()), &wr, nullptr);
                CloseHandle(h);
            }
        }
    };

    peLog(L"=== OneTimePE WinPE Auto-Network Engine Started ===");
    peLog(L"RunnerDir: " + appDir + L" | ResolvedDir: " + resolvedDir);

    // 1. Recursively load all exported .inf drivers in NetConfig\Drivers via drvload.exe
    auto loadInfsRecursive = [&](auto& self, const std::wstring& dir) -> void {
        DWORD attr = GetFileAttributesW(dir.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;

        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring full = JoinPath(dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                self(self, full);
            } else {
                std::wstring low = ToLower(fd.cFileName);
                if (low.size() >= 4 && low.substr(low.size() - 4) == L".inf") {
                    peLog(L"Loading NIC driver: " + full);
                    RunHiddenProcess(L"drvload.exe", L"\"" + full + L"\"", peLog);
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    };
    loadInfsRecursive(loadInfsRecursive, drvDir);

    // 2. Initialize WinPE networking stack
    peLog(L"Initializing WinPE network stack (wpeutil InitializeNetwork)...");
    RunHiddenProcess(L"wpeutil.exe", L"InitializeNetwork", peLog);

    // 3. Import Wi-Fi XML profiles if present (and start wlansvc if available in custom PE)
    bool anyWifiXml = false;
    {
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW(JoinPath(wifiDir, L"*.xml").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            anyWifiXml = true;
            RunHiddenProcess(L"net.exe", L"start wlansvc", nullptr);
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::wstring xmlPath = JoinPath(wifiDir, fd.cFileName);
                    peLog(L"Importing Wi-Fi profile XML: " + xmlPath);
                    RunHiddenProcess(L"netsh.exe",
                                     L"wlan add profile filename=\"" + xmlPath + L"\" user=all",
                                     peLog);
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }

    if (GetFileAttributesW(cfgIni.c_str()) == INVALID_FILE_ATTRIBUTES) {
        peLog(L"netconfig.ini not found; DHCP initialization completed.");
        return 0;
    }

    int count = static_cast<int>(GetPrivateProfileIntW(L"General", L"AdapterCount", 0, cfgIni.c_str()));
    peLog(L"Applying configurations for " + std::to_wstring(count) + L" adapter(s)...");

    auto findPeFriendlyNameByMac = [](const std::wstring& targetMacUpper) -> std::wstring {
        ULONG bufLen = 32768;
        std::vector<BYTE> buf(bufLen);
        if (GetAdaptersAddresses(AF_UNSPEC, 0, nullptr,
                                 reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &bufLen) == ERROR_BUFFER_OVERFLOW) {
            buf.resize(bufLen);
        }
        if (GetAdaptersAddresses(AF_UNSPEC, 0, nullptr,
                                 reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &bufLen) != NO_ERROR) {
            return L"";
        }
        for (auto* aa = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()); aa; aa = aa->Next) {
            if (aa->PhysicalAddressLength >= 6) {
                std::wstring mac = ToUpper(FormatMacDash(aa->PhysicalAddress, aa->PhysicalAddressLength));
                if (mac == targetMacUpper && aa->FriendlyName) {
                    return aa->FriendlyName;
                }
            }
        }
        return L"";
    };

    for (int i = 0; i < count; ++i) {
        std::wstring sec = L"Adapter" + std::to_wstring(i);
        auto rd = [&](const wchar_t* key) {
            wchar_t b[512]{};
            GetPrivateProfileStringW(sec.c_str(), key, L"", b, _countof(b), cfgIni.c_str());
            return Trim(b);
        };

        std::wstring mac      = ToUpper(rd(L"MacAddress"));
        bool isWifi           = rd(L"IsWifi") == L"1";
        bool staticIp         = rd(L"StaticIP") == L"1";
        std::wstring ip       = rd(L"IPv4");
        std::wstring mask     = rd(L"SubnetMask");
        std::wstring gw       = rd(L"Gateway");
        bool staticDns        = rd(L"StaticDNS") == L"1";
        std::wstring dns1     = rd(L"DNS1");
        std::wstring dns2     = rd(L"DNS2");
        std::wstring wifiProf = rd(L"WifiProfile");

        if (mac.empty()) continue;

        // Poll up to 20 seconds for PnP to register the NIC in PE
        std::wstring peIfName;
        for (int attempt = 0; attempt < 20; ++attempt) {
            peIfName = findPeFriendlyNameByMac(mac);
            if (!peIfName.empty()) break;
            Sleep(1000);
        }

        if (peIfName.empty()) {
            peLog(L"Adapter with MAC " + mac + L" did not appear within timeout.");
            continue;
        }
        peLog(L"Matched MAC " + mac + L" -> PE Interface \"" + peIfName + L"\"");

        if ((isWifi || anyWifiXml) && !wifiProf.empty()) {
            RunHiddenProcess(L"netsh.exe",
                             L"wlan connect name=\"" + wifiProf + L"\" interface=\"" + peIfName + L"\"",
                             peLog);
            Sleep(3000);
        }

        if (staticIp && !ip.empty() && !mask.empty()) {
            std::wstring addrArgs = L"interface ipv4 set address name=\"" + peIfName +
                                    L"\" static " + ip + L" " + mask;
            if (!gw.empty()) addrArgs += L" " + gw + L" 1";
            RunHiddenProcess(L"netsh.exe", addrArgs, peLog);
        } else {
            // Ensure DHCP is active
            RunHiddenProcess(L"netsh.exe",
                             L"interface ipv4 set address name=\"" + peIfName + L"\" dhcp",
                             nullptr);
        }

        if (staticDns && !dns1.empty()) {
            RunHiddenProcess(L"netsh.exe",
                             L"interface ipv4 set dnsservers name=\"" + peIfName +
                             L"\" static " + dns1 + L" primary validate=no",
                             peLog);
            if (!dns2.empty()) {
                RunHiddenProcess(L"netsh.exe",
                                 L"interface ipv4 add dnsservers name=\"" + peIfName +
                                 L"\" " + dns2 + L" index=2 validate=no",
                                 peLog);
            }
        }
    }

    peLog(L"=== OneTimePE WinPE Auto-Network Completed ===");
    return 0;
}

} // namespace net_sync
