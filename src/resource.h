#pragma once

#define IDI_APP_ICON            101

// Custom window messages for background tasks (e.g., DISM WIM patching)
#define WM_APP_TASK_LOG         (WM_APP + 10)
#define WM_APP_TASK_DONE        (WM_APP + 11)

// Group 1: PE Files
#define IDC_EDT_APPDIR          1001
#define IDC_BTN_OPENDIR         1002
#define IDC_BTN_RESCAN          1003
#define IDC_CMB_WIM             1004
#define IDC_BTN_BROWSE_WIM      1005
#define IDC_CMB_SDI             1006
#define IDC_BTN_BROWSE_SDI      1007
#define IDC_BTN_GEN_SDI         1008
#define IDC_LBL_FILESTATUS      1009
#define IDC_BTN_IMPORT_CUSTOM   1010

// Group 2: Boot Variables & Security
#define IDC_EDT_DESC            1101
#define IDC_EDT_SYSROOT         1102
#define IDC_CMB_LOADER          1103
#define IDC_CHK_WINPE           1104
#define IDC_CHK_DETECTHAL       1105
#define IDC_CMB_NX              1106
#define IDC_CHK_BITLOCKER       1107
#define IDC_EDT_BL_COUNT        1108
#define IDC_EDT_REBOOT_SEC      1109
#define IDC_EDT_EXTRA_SET       1110
#define IDC_LBL_SECSTATUS       1111
#define IDC_BTN_CHECK_BL        1112

// Group 3: PE Auto-Network & WIM Unattended Patch
#define IDC_LBL_NETSTATUS       1151
#define IDC_CHK_AUTONET         1152
#define IDC_CHK_EXPORT_DRV      1153
#define IDC_BTN_SYNC_NET        1154
#define IDC_BTN_PATCH_WIM       1155

// Group 4: Status & Actions
#define IDC_LBL_BCDSTATUS       1201
#define IDC_BTN_REFRESH_BCD     1202
#define IDC_BTN_SAVE_CFG        1203
#define IDC_BTN_APPLY_BCD       1204
#define IDC_BTN_QUEUE_ONLY      1205
#define IDC_BTN_CLEAN_BCD       1206
#define IDC_BTN_BOOT_NOW        1207

// Group 5: Log
#define IDC_EDT_LOG             1301
#define IDC_BTN_CLEAR_LOG       1302

// Header Labels
#define IDC_LBL_TITLE           1401
#define IDC_LBL_SYSINFO         1402

// Custom PE Import Dialog Controls
#define IDC_DLG_BTN_PICK_ISO    1501
#define IDC_DLG_BTN_PICK_DIR    1502
#define IDC_DLG_PE_LIST         1503
#define IDC_DLG_BTN_OK          1504
#define IDC_DLG_BTN_CANCEL      1505
#define IDC_DLG_CHK_FIX_DRV     1506
