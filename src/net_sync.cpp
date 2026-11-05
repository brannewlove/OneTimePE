#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wlanapi.h>

#include "net_sync.h"

#include <algorithm>
#include <cwctype>
#include <cstdio>
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

    auto pOpen  = reinterpret_cast<FnWlanOpenHandle>(GetProcAddress(hWlan, "WlanOpenHandle"));
    auto pEnum  = reinterpret_cast<FnWlanEnumInterfaces>(GetProcAddress(hWlan, "WlanEnumInterfaces"));
    auto pQuery = reinterpret_cast<FnWlanQueryInterface>(GetProcAddress(hWlan, "WlanQueryInterface"));
    auto pFree  = reinterpret_cast<FnWlanFreeMemory>(GetProcAddress(hWlan, "WlanFreeMemory"));
    auto pClose = reinterpret_cast<FnWlanCloseHandle>(GetProcAddress(hWlan, "WlanCloseHandle"));

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
