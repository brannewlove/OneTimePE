#pragma once

#include <windows.h>
#include <cstdint>
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

struct WimInspectionInfo {
    bool         fileOpened    = false;
    bool         validWimMagic = false;
    bool         isLzmsEsd     = false; // LZMS solid compression (.esd renamed to .wim -> 0xc000000f)
    bool         isSplitSwm    = false; // Spanned WIM (usTotalParts > 1)
    uint32_t     imageCount    = 0;
    uint32_t     bootIndex     = 0;     // 0 means no bootable image marked in WIM header
    int          arch          = -1;    // 0 = x86, 9 = x64, 12 = ARM64, -1 = unknown
    int          osBuild       = 0;     // e.g. 19041, 22631, 26100
    std::wstring systemRoot;            // e.g. "\\WINDOWS"
    std::wstring imageName;
};

struct CustomPeBootEntry {
    std::wstring      bcdGuid;
    std::wstring      description;
    std::wstring      wimRelPath;       // e.g. "\\sources\\boot.wim" or "\\SSTR\\strelec10x64Eng.wim"
    std::wstring      wimFullPath;      // resolved full path on media
    std::wstring      sdiRelPath;       // e.g. "\\boot\\boot.sdi"
    std::wstring      sdiFullPath;      // resolved full path on media
    std::wstring      loaderPath;       // e.g. "\\windows\\system32\\boot\\winload.efi"
    std::wstring      systemRoot;       // e.g. "\\windows"
    bool              hasWinpeFlag     = false;
    bool              winpeMode        = true;
    bool              hasDetectHalFlag = false;
    bool              detectHal        = true;
    std::wstring      nxPolicy;         // "", "OptIn", "OptOut", "AlwaysOff", "AlwaysOn"
    std::wstring      extraSetArgs;     // e.g. "nointegritychecks yes; testsigning yes"
    bool              hasNoIntegrity   = false;
    bool              hasTestSigning   = false;
    bool              isDefault        = false;
    int               displayOrderRank = 9999;
    WimInspectionInfo wimInfo;
};

struct CustomPeDiscoveryResult {
    bool                           ok = false;
    std::wstring                   errorMsg;
    std::wstring                   mediaRoot;
    std::wstring                   usedBcdPath;
    bool                           synthesizedWithoutBcd = false;
    std::vector<CustomPeBootEntry> entries;
    std::vector<std::wstring>      externalToolMarkers;
};

struct IsoMountSession {
    bool         mounted        = false;
    bool         mountedByUs    = false;
    bool         usedPowerShell = false;
    std::wstring isoFullPath;
    std::wstring driveRoot;     // e.g. "E:\\"
    HANDLE       vhdHandle      = INVALID_HANDLE_VALUE;
};

// Fast (non-blocking) detection of active Windows network adapters, drivers, and Wi-Fi
DetectedNetworkSummary DetectActiveWindowsNetwork();

// Compute signature ("size:highTime:lowTime") of a WIM file to track patch status
std::wstring ComputeFileSignature(const std::wstring& fullPath);

// Fast (<1ms) binary header + XML metadata inspection of a WIM file
WimInspectionInfo InspectWimFile(const std::wstring& wimFullPath);

// Ensure WIM header has a valid BootIndex (via wimgapi.dll or DISM export fallback)
bool EnsureWimBootableFlag(const std::wstring& wimFullPath,
                           uint32_t targetIndex,
                           const LogCallback& logCb);

// Convert an LZMS/ESD-compressed .wim file into a standard RAMDisk-bootable LZX .wim via DISM
bool ConvertEsdWimToBootableWim(const std::wstring& wimFullPath,
                                uint32_t sourceIndex,
                                const LogCallback& logCb,
                                std::wstring& outNewWimSig);

// Probe inside WIM via wimgapi.dll to check whether winload is in System32 or System32\Boot
std::wstring DetectPreferredLoaderPathInWim(const std::wstring& wimFullPath,
                                            uint32_t imageIndex,
                                            bool isUefi,
                                            const std::wstring& currentLoaderPath);

// Fast probe via wimgapi.dll to check if OneTimePE unattended helper (C: drive fix + boot hook) is already inside WIM
bool IsWimUnattendedPatched(const std::wstring& wimFullPath, uint32_t imageIndex = 1);

// Mount an ISO image read-only (dynamically via virtdisk.dll with PowerShell fallback)
bool MountIsoReadOnly(const std::wstring& isoFullPath,
                      IsoMountSession& outSession,
                      const LogCallback& logCb);

// Unmount an ISO session if it was mounted by OneTimePE
void UnmountIsoSession(IsoMountSession& session,
                       const LogCallback& logCb);

// Parse EFI\Microsoft\Boot\BCD (or boot\bcd / folder scan) and discover Custom PE WIM + SDI + flags
CustomPeDiscoveryResult AnalyzeCustomPeMedia(const std::wstring& selectedPath,
                                             bool preferUefi,
                                             const LogCallback& logCb);

// Snapshot current internal Windows drive letters (C:, D:, ...) + Volume GUIDs & update bridge before PE reboot
bool ExportDriveMapForPe(const std::wstring& appBcdRelDir,
                         bool autoApplyNetwork,
                         const LogCallback& logCb);

// Export current Windows network settings, Wi-Fi XML, NIC drivers, and bridge pointer
bool ExportNetworkConfigForPe(const std::wstring& appDir,
                              const std::wstring& appBcdRelDir,
                              bool exportDrivers,
                              const LogCallback& logCb,
                              std::wstring& outSummaryReport);

// Mount boot.wim via DISM, inject C: drive auto-restore + unattended auto-network helper & hooks, and commit
bool PatchBootWimUnattended(const std::wstring& appDir,
                            const std::wstring& wimFullPath,
                            const LogCallback& logCb,
                            std::wstring& outNewWimSig);

// Executed silently inside WinPE to unlock hidden/offline attributes and restore C: (and fixed drives)
int RunPeFixDrivesMode();

// Executed silently inside WinPE at startup ("--pe-boot-init"): fixes C: drive map first, then applies network if enabled
int RunPeBootInitMode(const std::wstring& appDir);

// Executed silently inside WinPE when launched with "--pe-apply-net"
int RunPeAutoNetMode(const std::wstring& appDir);

} // namespace net_sync

