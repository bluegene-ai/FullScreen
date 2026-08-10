// ============================================================
// test_radio_group.cpp - runnable self-check for the "Auto refresh"
// radio group mutual exclusion (Win32 dialog-manager behavior).
//
// Build (VS2022 x64 dev prompt, after a successful build.bat):
//   cl /nologo /W4 /EHsc /std:c++17 test_radio_group.cpp ^
//       /I..\src /Fe:test_radio_group.exe ^
//       ..\build\resource.res user32.lib
// Loads IDD_CONFIG_DIALOG, simulates a click on each radio button
// in turn, and asserts that exactly one is checked at a time.
// ============================================================
#include <Windows.h>
#include <cstdio>
#include "resource.h"

static INT_PTR CALLBACK TestProc(HWND, UINT, WPARAM, LPARAM)
{
    return FALSE; // hand everything to DefDlgProc
}

int main()
{
    HWND hDlg = CreateDialogParamW(GetModuleHandleW(nullptr),
                                   MAKEINTRESOURCEW(IDD_CONFIG_DIALOG),
                                   nullptr, TestProc, 0);
    if (!hDlg) {
        std::printf("CreateDialog failed (GetLastError=%lu)\n", GetLastError());
        return 1;
    }

    const int ids[3] = { IDC_RADIO_REFRESH_OFF,
                         IDC_RADIO_REFRESH_INTERVAL,
                         IDC_RADIO_REFRESH_DAILY };
    const char* names[3] = { "Off", "Every", "Daily at" };
    bool ok = true;

    std::printf("Radio group mutual-exclusion check\n\n");
    for (int i = 0; i < 3; i++) {
        // Simulate a real user click (drives BN_CLICKED -> dialog manager).
        SendMessageW(GetDlgItem(hDlg, ids[i]), BM_CLICK, 0, 0);

        for (int j = 0; j < 3; j++) {
            bool checked = IsDlgButtonChecked(hDlg, ids[j]) == BST_CHECKED;
            bool expect = (i == j);
            std::printf("  after clicking '%s' -> '%s' is %s (expect %s) [%s]\n",
                        names[i], names[j],
                        checked ? "checked" : "unchecked",
                        expect ? "checked" : "unchecked",
                        checked == expect ? "PASS" : "FAIL");
            if (checked != expect) ok = false;
        }
    }

    DestroyWindow(hDlg);
    std::printf("\n%s\n", ok ? "All checks passed." : "FAILED.");
    return ok ? 0 : 1;
}
