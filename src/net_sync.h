#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <functional>

namespace net_sync {

using LogCallback = std::function<void(const std::wstring&)>;

struct AdapterConfig {
    bool         valid        = false;
    bool         isWifi       = false;
    std::wstring friendlyName;
    std::wstring hardwareDesc;
    std::wstring adapterGuid;
    std::wstring macDash;      // e.g. "18-C0-4D-5A-19-2A"
    std::wstring infName;      // e.g. "oem42.inf"
    bool         staticIp     = false;
    std::wstring ipv4;
    std::wstring subnetMask;
    std::wstring gateway;
    bool         staticDns    = false;
    std::wstring dns1;
    std::wstring dns2;
    std::wstring wifiProfile;
};

struct DetectedNetworkSummary {
    bool                         foundAny = false;
    std::vector<AdapterConfig>   adapters;
    std::vector<std::wstring>    physicalOemInfs;
    std::wstring                 shortBannerText;
};

// Fast (non-blocking) detection of active Windows network adapters, drivers, and Wi-Fi
DetectedNetworkSummary DetectActiveWindowsNetwork();

// Compute signature ("size:highTime:lowTime") of a WIM file to track patch status
std::wstring ComputeFileSignature(const std::wstring& fullPath);

// Export current Windows network settings, Wi-Fi XML, NIC drivers, and bridge pointer
bool ExportNetworkConfigForPe(const std::wstring& appDir,
                              const std::wstring& appBcdRelDir,
                              bool exportDrivers,
                              const LogCallback& logCb,
                              std::wstring& outSummaryReport);

// Mount boot.wim via DISM, inject unattended auto-network bootstrap hooks, and commit
bool PatchBootWimUnattended(const std::wstring& appDir,
                            const std::wstring& wimFullPath,
                            const LogCallback& logCb,
                            std::wstring& outNewWimSig);

// Executed silently inside WinPE when launched with "--pe-apply-net"
int RunPeAutoNetMode(const std::wstring& appDir);

} // namespace net_sync

