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

#include "net_sync.h"

#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <cstring>
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
        wcsncpy_s(physNtTarget, physPath.c_str(), _TRUNCATE);
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

    // 5. Write UTF-16LE Unicode bridge & copy self to %SystemRoot%\Temp\OneTimePE_Runner.exe
    //    Guarantees 100% path resolution in English/Korean WinPE even with Korean/spaces in folder names!
    wchar_t winDir[MAX_PATH]{};
    if (GetWindowsDirectoryW(winDir, MAX_PATH) > 0 && !appBcdRelDir.empty()) {
        std::wstring tempDir = JoinPath(winDir, L"Temp");
        EnsureDirectoryRecursive(tempDir);

        std::wstring bridgeIni = JoinPath(tempDir, L"OneTimePE_Bridge.ini");
        DeleteFileW(bridgeIni.c_str());
        EnsureUnicodeIni(bridgeIni);
        WritePrivateProfileStringW(L"Bridge", L"RelAppDir", appBcdRelDir.c_str(), bridgeIni.c_str());

        wchar_t selfExe[MAX_PATH * 4]{};
        if (GetModuleFileNameW(nullptr, selfExe, _countof(selfExe)) > 0) {
            std::wstring runnerDst = JoinPath(tempDir, L"OneTimePE_Runner.exe");
            CopyFileW(selfExe, runnerDst.c_str(), FALSE);
        }

        std::wstring bridgeCmd = JoinPath(tempDir, L"OneTimePE_NetBridge.cmd");
        std::string relOem = WideToOemOrAcp(appBcdRelDir, GetOEMCP());
        WriteAsciiFile(bridgeCmd, "@set \"OTPE_REL=" + relOem + "\"\r\n");

        if (logCb) logCb(L"무인 부팅 유니코드 브리지 갱신 완료 -> " + appBcdRelDir);
    }

    outSummaryReport += L"\n저장 폴더: " + netDir;
    if (logCb) logCb(L"네트워크 설정 및 드라이버 추출이 완료되었습니다.");
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
    if (logCb) logCb(L"--- WIM 무인 네트워크 자동 실행 패치 시작 ---");
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

    // Clean up any stale mount state before starting
    RunHiddenProcess(L"dism.exe", L"/English /Cleanup-Mountpoints", nullptr);
    if (GetFileAttributesW(mountDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        RunHiddenProcess(L"dism.exe", L"/English /Unmount-Image /MountDir:\"" + mountDir + L"\" /Discard", nullptr);
        DeleteDirectoryContentsRecursive(mountDir);
        RemoveDirectoryW(mountDir.c_str());
    }
    EnsureDirectoryRecursive(mountDir);

    // Bootstrapper script placed inside X:\Windows\System32\OneTimePE_AutoNet.cmd
    const std::string autoNetScript =
        "@echo off\r\n"
        "setlocal\r\n"
        "if exist \"X:\\Windows\\Temp\\OneTimePE_NetDone.flg\" exit /b 0\r\n"
        "if not exist \"X:\\Windows\\Temp\" mkdir \"X:\\Windows\\Temp\" >nul 2>&1\r\n"
        "echo 1 > \"X:\\Windows\\Temp\\OneTimePE_NetDone.flg\"\r\n"
        "\r\n"
        "mountvol /e >nul 2>&1\r\n"
        "set /a OTPE_TRY=0\r\n"
        "\r\n"
        ":FIND_LOOP\r\n"
        "set /a OTPE_TRY+=1\r\n"
        "\r\n"
        "for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (\r\n"
        "    if exist \"%%D:\\Windows\\Temp\\OneTimePE_Runner.exe\" (\r\n"
        "        \"%%D:\\Windows\\Temp\\OneTimePE_Runner.exe\" --pe-apply-net\r\n"
        "        exit /b 0\r\n"
        "    )\r\n"
        ")\r\n"
        "\r\n"
        "set \"OTPE_REL=\"\r\n"
        "set \"OTPE_EXE=\"\r\n"
        "set \"OTPE_CMD=\"\r\n"
        "\r\n"
        "for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (\r\n"
        "    if exist \"%%D:\\Windows\\Temp\\OneTimePE_NetBridge.cmd\" (\r\n"
        "        call \"%%D:\\Windows\\Temp\\OneTimePE_NetBridge.cmd\"\r\n"
        "    )\r\n"
        ")\r\n"
        "\r\n"
        "if defined OTPE_REL (\r\n"
        "    for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (\r\n"
        "        if exist \"%%D:%OTPE_REL%\\OneTimePE.exe\" set \"OTPE_EXE=%%D:%OTPE_REL%\\OneTimePE.exe\"\r\n"
        "        if exist \"%%D:%OTPE_REL%\\NetConfig\\ApplyPENetwork.cmd\" set \"OTPE_CMD=%%D:%OTPE_REL%\\NetConfig\\ApplyPENetwork.cmd\"\r\n"
        "    )\r\n"
        ")\r\n"
        "\r\n"
        "if not defined OTPE_EXE (\r\n"
        "    for %%D in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (\r\n"
        "        if exist \"%%D:\\PEBoot\\OneTimePE.exe\" set \"OTPE_EXE=%%D:\\PEBoot\\OneTimePE.exe\"\r\n"
        "        if exist \"%%D:\\OneTimePE\\PEBoot\\OneTimePE.exe\" set \"OTPE_EXE=%%D:\\OneTimePE\\PEBoot\\OneTimePE.exe\"\r\n"
        "        if exist \"%%D:\\WinPE\\OneTimePE.exe\" set \"OTPE_EXE=%%D:\\WinPE\\OneTimePE.exe\"\r\n"
        "    )\r\n"
        ")\r\n"
        "\r\n"
        "if defined OTPE_EXE (\r\n"
        "    \"%OTPE_EXE%\" --pe-apply-net\r\n"
        "    exit /b 0\r\n"
        ")\r\n"
        "if defined OTPE_CMD (\r\n"
        "    call \"%OTPE_CMD%\"\r\n"
        "    exit /b 0\r\n"
        ")\r\n"
        "\r\n"
        "if %OTPE_TRY% LSS 15 (\r\n"
        "    ping -n 3 127.0.0.1 >nul 2>&1\r\n"
        "    goto :FIND_LOOP\r\n"
        ")\r\n"
        "\r\n"
        "endlocal\r\n"
        "exit /b 0\r\n";

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

        std::wstring sys32Dir = JoinPath(mountDir, L"Windows\\System32");
        bool patchOk = true;

        // A. Write OneTimePE_AutoNet.cmd into System32
        std::wstring autoNetPath = JoinPath(sys32Dir, L"OneTimePE_AutoNet.cmd");
        if (!WriteAsciiFile(autoNetPath, autoNetScript)) {
            if (logCb) logCb(L"오류: OneTimePE_AutoNet.cmd 파일 기록 실패");
            patchOk = false;
        } else if (logCb) {
            logCb(L" • 주입 완료: \\Windows\\System32\\OneTimePE_AutoNet.cmd");
        }

        // B. Hook 1: \Windows\System32\startnet.cmd (Standard WinPE)
        if (patchOk) {
            std::wstring startnetPath = JoinPath(sys32Dir, L"startnet.cmd");
            std::wstring content;
            TextEncoding enc = TextEncoding::AsciiOrUtf8NoBom;
            if (!ReadTextAutoEncoding(startnetPath, content, enc)) {
                content = L"wpeinit\r\n";
                enc     = TextEncoding::AsciiOrUtf8NoBom;
            }
            if (ToLower(content).find(L"onetimepe_autonet.cmd") == std::wstring::npos) {
                if (!content.empty() && content.back() != L'\n') content += L"\r\n";
                content += L"@start \"\" /min \"%SystemRoot%\\System32\\OneTimePE_AutoNet.cmd\"\r\n";
                WriteTextWithEncoding(startnetPath, content, enc);
                if (logCb) logCb(L" • 후킹 완료: \\Windows\\System32\\startnet.cmd");
            } else if (logCb) {
                logCb(L" • 확인됨: startnet.cmd에 이미 자동 실행 훅이 존재함");
            }
        }

        // C. Hook 2: \Windows\System32\winpeshl.ini (Custom GUI / PECMD PE — ONLY if file already exists!)
        std::wstring winpeshlPath = JoinPath(sys32Dir, L"winpeshl.ini");
        if (patchOk && GetFileAttributesW(winpeshlPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring iniContent;
            TextEncoding enc = TextEncoding::AsciiOrUtf8NoBom;
            if (ReadTextAutoEncoding(winpeshlPath, iniContent, enc)) {
                if (ToLower(iniContent).find(L"onetimepe_autonet.cmd") == std::wstring::npos) {
                    std::wstring lowIni = ToLower(iniContent);
                    size_t secPos = lowIni.find(L"[launchapps]");
                    const std::wstring hookLine =
                        L"%SYSTEMROOT%\\System32\\cmd.exe, /c start \"\" /min \"%SystemRoot%\\System32\\OneTimePE_AutoNet.cmd\"\r\n";
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
                    logCb(L" • 확인됨: winpeshl.ini에 이미 자동 실행 훅이 존재함");
                }
            }
        }

        // D. Hook 3: Explorer Startup folder fallback for custom Explorer-shell PEs
        std::wstring startupDir = JoinPath(mountDir, L"Users\\Default\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup");
        if (patchOk && GetFileAttributesW(startupDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            WriteAsciiFile(JoinPath(startupDir, L"OneTimePE_AutoNet.cmd"), autoNetScript);
            if (logCb) logCb(L" • 후킹 완료: Default User Startup 시작프로그램 폴더");
        }

        // E. Ensure both System32\winload.{efi,exe} and System32\Boot\winload.{efi,exe} exist when one is present
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
    if (logCb) logCb(L"★ WIM 무인 네트워크 자동 실행 패치가 완료되었습니다!");
    return true;
}

// ============================================================================
// Silent execution engine inside WinPE: "OneTimePE.exe --pe-apply-net"
// ============================================================================
int RunPeAutoNetMode(const std::wstring& appDir) {
    std::wstring resolvedDir = appDir;
    if (GetFileAttributesW(JoinPath(resolvedDir, L"NetConfig\\netconfig.ini").c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring bridgeIni = JoinPath(resolvedDir, L"OneTimePE_Bridge.ini");
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
