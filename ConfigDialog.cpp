#include "ConfigDialog.h"
#include "AudioCapture.h"
#include <commctrl.h>
#include <dwmapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

// DWM attributes for Win10/Win11 visual polish — defined locally so we don't
// need a newer Windows SDK at build time.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
enum AC_DWMWCP { DWMWCP_DEFAULT_AC = 0, DWMWCP_DONOTROUND_AC = 1, DWMWCP_ROUND_AC = 2 };

static void ApplyWin11Look(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWORD corner = DWMWCP_ROUND_AC;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    // Mica backdrop is finicky with our custom-painted bg; leave default.
}

// Module-local lists so SaveAndClose can map combo selection -> device ID
static std::vector<AudioDeviceInfo> s_outDevices;
static std::vector<AudioDeviceInfo> s_micDevices;

// Module-local capture state — which rebind button is waiting for a keystroke
static int  s_captureAction = -1;
static HWND s_captureButton = NULL;

// API key that was accidentally committed to git history early in development.
// Warn the user at startup if it's still being used (they should rotate it).
static const char* kLeakedApiKey = "AIzaSyDfUjzN9eBoi2ZJb4VUc-AzokIFiNAYfbM";

LLMConfig* ConfigDialog::s_config = nullptr;
const std::vector<ModelInfo>* ConfigDialog::s_models = nullptr;
bool ConfigDialog::s_success = false;

static std::wstring Utf8To16(const std::string& s) {
    if (s.empty()) return std::wstring();
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], sz);
    return w;
}

// Per-button subclass: catches WM_KEYDOWN when this button is in capture mode.
static LRESULT CALLBACK RebindButtonProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR /*subclassId*/, DWORD_PTR refData)
{
    int actionIdx = (int)refData;

    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (s_captureAction != actionIdx) break;
        UINT vk = (UINT)wParam;
        // Ignore standalone modifier keys (wait for the real key)
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
            vk == VK_LWIN  || vk == VK_RWIN    || vk == VK_LSHIFT || vk == VK_RSHIFT ||
            vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU)
        {
            break;
        }
        // Escape cancels capture
        if (vk == VK_ESCAPE) {
            s_captureAction = -1;
            s_captureButton = NULL;
            // Restore label to current binding
            if (ConfigDialog::s_config) {
                const auto& b = ConfigDialog::s_config->hotkeys.bindings[actionIdx];
                SetWindowTextW(hwnd, Utf8To16(ConfigLoader::BindingToString(b)).c_str());
            }
            return 0;
        }
        UINT mods = 0;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
        if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= MOD_ALT;
        if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= MOD_SHIFT;

        if (ConfigDialog::s_config) {
            HotkeyBinding b = { mods, vk };
            ConfigDialog::s_config->hotkeys.bindings[actionIdx] = b;
            SetWindowTextW(hwnd, Utf8To16(ConfigLoader::BindingToString(b)).c_str());
        }
        s_captureAction = -1;
        s_captureButton = NULL;
        return 0;
    }
    case WM_KILLFOCUS:
        if (s_captureAction == actionIdx) {
            // Lost focus mid-capture — cancel and revert text
            if (ConfigDialog::s_config) {
                const auto& b = ConfigDialog::s_config->hotkeys.bindings[actionIdx];
                SetWindowTextW(hwnd, Utf8To16(ConfigLoader::BindingToString(b)).c_str());
            }
            s_captureAction = -1;
            s_captureButton = NULL;
        }
        break;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

bool ConfigDialog::Show(HINSTANCE hInstance, LLMConfig& config, const std::vector<ModelInfo>& models) {
    s_config = &config;
    s_models = &models;
    s_success = false;

    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = DialogProc;
    wc.hInstance = hInstance;
    // Dark backdrop for the welcome screen
    wc.hbrBackground = CreateSolidBrush(RGB(28, 28, 32));
    wc.lpszClassName = L"ConfigDialogClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    WNDCLASSEX wcExisting;
    if (!GetClassInfoEx(hInstance, wc.lpszClassName, &wcExisting)) {
        RegisterClass(&wc);
    }

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int winW = 700;
    int winH = 760;
    int x = (screenW - winW) / 2;
    int y = (screenH - winH) / 2;

    HWND hwnd = CreateWindow(wc.lpszClassName, L"Invisible AI Overlay",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, winW, winH,
        NULL, NULL, hInstance, NULL);

    if (!hwnd) return false;

    // Win11 polish: dark title bar + rounded corners (no-op on older Windows)
    ApplyWin11Look(hwnd);

    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (msg.message == WM_QUIT && msg.wParam == 1) {
        return true;
    }
    return false;
}

LRESULT CALLBACK ConfigDialog::DialogProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    static HBRUSH s_bgBrush = NULL;

    switch (uMsg) {
    case WM_CREATE:
        if (!s_bgBrush) s_bgBrush = CreateSolidBrush(RGB(28, 28, 32));
        InitializeControls(hwnd);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);

        // Header strip
        RECT hdr = { 0, 0, rc.right, 70 };
        HBRUSH hdrBg = CreateSolidBrush(RGB(20, 20, 26));
        FillRect(hdc, &hdr, hdrBg);
        DeleteObject(hdrBg);

        // Accent underline
        RECT line = { 0, 70, rc.right, 72 };
        HBRUSH lineBg = CreateSolidBrush(RGB(80, 160, 255));
        FillRect(hdc, &line, lineBg);
        DeleteObject(lineBg);

        SetBkMode(hdc, TRANSPARENT);

        // Brand
        HFONT hBig = CreateFont(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        HFONT oldF = (HFONT)SelectObject(hdc, hBig);
        SetTextColor(hdc, RGB(255, 255, 255));
        RECT brandRc = { 20, 10, rc.right, 45 };
        DrawText(hdc, L"✨ Invisible AI Overlay", -1, &brandRc,
                 DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

        HFONT hTag = CreateFont(14, 0, 0, 0, FW_NORMAL, TRUE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        SelectObject(hdc, hTag);
        SetTextColor(hdc, RGB(160, 200, 255));
        RECT tagRc = { 22, 44, rc.right, 65 };
        DrawText(hdc, L"Your live interview copilot — invisible to screen capture",
                 -1, &tagRc, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

        // Right panel header — actual rebind controls are real Win32 buttons added in InitializeControls
        int sheetX = 360;
        int sheetY = 84;
        HFONT hSheetHdr = CreateFont(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        SelectObject(hdc, hSheetHdr);
        SetTextColor(hdc, RGB(120, 200, 255));
        RECT shRc = { sheetX, sheetY, rc.right - 20, sheetY + 22 };
        DrawText(hdc, L"Hotkeys (click to rebind)", -1, &shRc, DT_LEFT | DT_SINGLELINE);

        // Fixed-keys note (under the rebindables)
        HFONT hNote = CreateFont(12, 0, 0, 0, FW_NORMAL, TRUE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        SelectObject(hdc, hNote);
        SetTextColor(hdc, RGB(140, 140, 140));
        RECT noteRc = { sheetX, rc.bottom - 70, rc.right - 20, rc.bottom - 18 };
        DrawText(hdc,
            L"Fixed: INS = text, DEL = hide, END = exit, PgUp/Dn = scroll.\nEsc cancels rebind.",
            -1, &noteRc, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

        SelectObject(hdc, oldF);
        DeleteObject(hBig);
        DeleteObject(hTag);
        DeleteObject(hSheetHdr);
        DeleteObject(hNote);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, s_bgBrush ? s_bgBrush : (HBRUSH)(COLOR_WINDOW + 1));
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        SetTextColor(hdcStatic, RGB(220, 220, 220));
        SetBkColor(hdcStatic, RGB(28, 28, 32));
        return (LRESULT)(s_bgBrush ? s_bgBrush : (HBRUSH)(COLOR_WINDOW + 1));
    }
    case WM_CTLCOLOREDIT: {
        // Dark input field — bright text on a slightly-lighter-than-bg fill
        static HBRUSH s_editBrush = NULL;
        if (!s_editBrush) s_editBrush = CreateSolidBrush(RGB(42, 42, 50));
        HDC hdcEdit = (HDC)wParam;
        SetTextColor(hdcEdit, RGB(240, 240, 245));
        SetBkColor(hdcEdit, RGB(42, 42, 50));
        return (LRESULT)s_editBrush;
    }
    case WM_CTLCOLORLISTBOX: {
        // Combo dropdown lists
        static HBRUSH s_listBrush = NULL;
        if (!s_listBrush) s_listBrush = CreateSolidBrush(RGB(42, 42, 50));
        HDC hdcList = (HDC)wParam;
        SetTextColor(hdcList, RGB(240, 240, 245));
        SetBkColor(hdcList, RGB(42, 42, 50));
        return (LRESULT)s_listBrush;
    }

    case WM_COMMAND: {
        int cmd = LOWORD(wParam);
        if (cmd == ID_BTN_START) {
            SaveAndClose(hwnd);
        }
        if (cmd == ID_BTN_CLEAR_KEY) {
            SetWindowTextW(GetDlgItem(hwnd, ID_EDIT_APIKEY), L"");
            SetFocus(GetDlgItem(hwnd, ID_EDIT_APIKEY));
        }
        if (cmd == ID_BTN_DEL_SESSION) {
            // Get current session name from combo, delete its chat file, switch to default
            HWND hSess = GetDlgItem(hwnd, ID_EDIT_SESSION);
            char buf[256] = {0};
            GetWindowTextA(hSess, buf, sizeof(buf));
            std::string sn = buf;
            if (!sn.empty() && sn != "default") {
                std::wstring prompt = L"Delete session '" + Utf8To16(sn) + L"' and its chat history?";
                if (MessageBoxW(hwnd, prompt.c_str(), L"Delete session", MB_ICONWARNING | MB_OKCANCEL) == IDOK) {
                    std::wstring path = L"chat." + Utf8To16(sn) + L".txt";
                    DeleteFileW(path.c_str());
                    // Remove from combo + reset selection to default
                    int idx = (int)SendMessageW(hSess, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)Utf8To16(sn).c_str());
                    if (idx != CB_ERR) SendMessage(hSess, CB_DELETESTRING, idx, 0);
                    SetWindowTextA(hSess, "default");
                }
            } else {
                MessageBoxW(hwnd, L"The 'default' session can't be deleted.", L"Delete session", MB_ICONINFORMATION | MB_OK);
            }
        }
        if (cmd == ID_BTN_RESET_KEYS) {
            s_config->hotkeys = ConfigLoader::DefaultHotkeys();
            // Refresh each rebind button's label
            for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
                HWND b = GetDlgItem(hwnd, ID_REBIND_BASE + i);
                if (b) {
                    SetWindowTextW(b, Utf8To16(ConfigLoader::BindingToString(
                        s_config->hotkeys.bindings[i])).c_str());
                }
            }
        }
        if (cmd == ID_COMBO_PROVIDER && HIWORD(wParam) == CBN_SELCHANGE) {
            OnProviderChanged(hwnd);
        }
        if (cmd >= ID_REBIND_BASE && cmd < ID_REBIND_BASE + (int)HotkeyAction::Count
            && HIWORD(wParam) == BN_CLICKED)
        {
            int idx = cmd - ID_REBIND_BASE;
            s_captureAction = idx;
            s_captureButton = (HWND)lParam;
            SetWindowTextW(s_captureButton, L"Press key...");
            SetFocus(s_captureButton);
        }
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(s_success ? 1 : 0);
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void ConfigDialog::InitializeControls(HWND hwnd) {
    if (!s_config) return;

    HFONT hFont = CreateFont(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    // Form starts below the 72px banner; left column only.
    int y = 90;
    const int X = 20;
    const int W = 320;  // form column width
    const int LBL_H = 18, FLD_H = 25, GAP = 6;

    auto mkLabel = [&](const wchar_t* text, int id = 0) {
        HWND h = CreateWindow(L"STATIC", text, WS_VISIBLE | WS_CHILD,
            X, y, W, LBL_H, hwnd, (HMENU)(INT_PTR)id, NULL, NULL);
        SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += LBL_H + 2;
        return h;
    };

    // ---- Provider ----
    mkLabel(L"Provider");
    HWND hProv = CreateWindow(L"COMBOBOX", NULL,
        WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        X, y, W, 200, hwnd, (HMENU)ID_COMBO_PROVIDER, NULL, NULL);
    SendMessage(hProv, WM_SETFONT, (WPARAM)hFont, TRUE);
    int selectedProv = 0;
    const auto& providers = ConfigLoader::BuiltinProviders();
    for (size_t i = 0; i < providers.size(); ++i) {
        std::string display = providers[i].name;
        if (providers[i].supportsAudio) display += "  (audio supported)";
        SendMessageW(hProv, CB_ADDSTRING, 0, (LPARAM)Utf8To16(display).c_str());
        if (providers[i].id == s_config->provider) selectedProv = (int)i;
    }
    SendMessage(hProv, CB_SETCURSEL, selectedProv, 0);
    y += FLD_H + GAP;

    // ---- Model ----
    mkLabel(L"Model");
    HWND hModel = CreateWindow(L"COMBOBOX", NULL,
        WS_VISIBLE | WS_CHILD | CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP,
        X, y, W, 220, hwnd, (HMENU)ID_COMBO_MODEL, NULL, NULL);
    SendMessage(hModel, WM_SETFONT, (WPARAM)hFont, TRUE);
    y += FLD_H + GAP;

    // ---- API Key (with Clear button) ----
    mkLabel(L"API Key");
    HWND hEdit = CreateWindow(L"EDIT", NULL,
        WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD,
        X, y, W - 65, FLD_H, hwnd, (HMENU)ID_EDIT_APIKEY, NULL, NULL);
    SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    SetWindowTextA(hEdit, s_config->api_key.c_str());

    HWND hClearBtn = CreateWindow(L"BUTTON", L"Clear",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        X + W - 60, y, 60, FLD_H, hwnd, (HMENU)ID_BTN_CLEAR_KEY, NULL, NULL);
    SendMessage(hClearBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

    // If the leaked key is currently in use, show a warning label below the input
    if (s_config->api_key == kLeakedApiKey) {
        y += FLD_H + 2;
        HWND hWarn = CreateWindow(L"STATIC",
            L"⚠ This key was exposed in git history. Rotate it at aistudio.google.com.",
            WS_VISIBLE | WS_CHILD,
            X, y, W, LBL_H, hwnd, NULL, NULL, NULL);
        SendMessage(hWarn, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    y += FLD_H + GAP;

    // ---- Base URL (Custom only) ----
    HWND hLblBase = CreateWindow(L"STATIC", L"Base URL (Custom provider only)",
        WS_CHILD, X, y, W, LBL_H, hwnd, (HMENU)ID_LBL_BASEURL, NULL, NULL);
    SendMessage(hLblBase, WM_SETFONT, (WPARAM)hFont, TRUE);
    int baseLblY = y;
    y += LBL_H + 2;

    HWND hBase = CreateWindow(L"EDIT", NULL,
        WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
        X, y, W, FLD_H, hwnd, (HMENU)ID_EDIT_BASEURL, NULL, NULL);
    SendMessage(hBase, WM_SETFONT, (WPARAM)hFont, TRUE);
    SetWindowTextA(hBase, s_config->base_url.c_str());
    (void)baseLblY;
    y += FLD_H + GAP;

    // ---- Session picker (per-session chat persistence) ----
    mkLabel(L"Session (pick existing, or type to create)");
    HWND hSession = CreateWindow(L"COMBOBOX", NULL,
        WS_VISIBLE | WS_CHILD | CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP,
        X, y, W - 70, 200, hwnd, (HMENU)ID_EDIT_SESSION, NULL, NULL);
    SendMessage(hSession, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Populate from existing chat.<name>.txt files in the working directory
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(L"chat.*.txt", &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name.size() > 9
                && name.compare(0, 5, L"chat.") == 0
                && name.compare(name.size() - 4, 4, L".txt") == 0)
            {
                std::wstring sn = name.substr(5, name.size() - 9);
                SendMessageW(hSession, CB_ADDSTRING, 0, (LPARAM)sn.c_str());
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    // Always offer "default" so users have a starting session
    if (SendMessageW(hSession, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)L"default") == CB_ERR) {
        SendMessageW(hSession, CB_ADDSTRING, 0, (LPARAM)L"default");
    }
    SetWindowTextA(hSession, s_config->session_name.c_str());

    HWND hDelSess = CreateWindow(L"BUTTON", L"Delete",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        X + W - 65, y, 65, FLD_H, hwnd, (HMENU)ID_BTN_DEL_SESSION, NULL, NULL);
    SendMessage(hDelSess, WM_SETFONT, (WPARAM)hFont, TRUE);
    y += FLD_H + GAP;

    // ---- Gemini fallback key (used to route F7/auto audio to Gemini when primary provider ≠ Gemini) ----
    mkLabel(L"Gemini key for audio (optional)");
    HWND hGem = CreateWindow(L"EDIT", NULL,
        WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD,
        X, y, W, FLD_H, hwnd, (HMENU)ID_EDIT_GEMINI_KEY, NULL, NULL);
    SendMessage(hGem, WM_SETFONT, (WPARAM)hFont, TRUE);
    SetWindowTextA(hGem, s_config->gemini_fallback_key.c_str());
    y += FLD_H + GAP * 2;

    // ---- Start button (primary action — visually prominent) ----
    HFONT hBtnFont = CreateFont(17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    int btnW = W;     // full form column width
    int btnH = 44;    // taller for visual weight
    HWND hBtn = CreateWindow(L"BUTTON", L"Start Overlay  ➜",
        WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        X, y + 6, btnW, btnH, hwnd, (HMENU)ID_BTN_START, NULL, NULL);
    SendMessage(hBtn, WM_SETFONT, (WPARAM)hBtnFont, TRUE);

    // ---- Rebind controls in the right panel ----
    int rbY = 110;
    const int rbX_lbl = 360;
    const int rbX_btn = 530;
    const int rbW_lbl = 165;
    const int rbW_btn = 70;
    const int rbH     = 24;
    HFONT hSmall = CreateFont(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
        HotkeyAction a = (HotkeyAction)i;
        std::wstring label = Utf8To16(ConfigLoader::ActionLabel(a));
        std::wstring bind  = Utf8To16(ConfigLoader::BindingToString(s_config->hotkeys.bindings[i]));

        HWND hLbl = CreateWindow(L"STATIC", label.c_str(),
            WS_VISIBLE | WS_CHILD,
            rbX_lbl, rbY + 3, rbW_lbl, rbH, hwnd, NULL, NULL, NULL);
        SendMessage(hLbl, WM_SETFONT, (WPARAM)hSmall, TRUE);

        HWND hBtn = CreateWindow(L"BUTTON", bind.c_str(),
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
            rbX_btn, rbY, rbW_btn, rbH, hwnd,
            (HMENU)(INT_PTR)(ID_REBIND_BASE + i), NULL, NULL);
        SendMessage(hBtn, WM_SETFONT, (WPARAM)hSmall, TRUE);
        SetWindowSubclass(hBtn, RebindButtonProc, 1, (DWORD_PTR)i);

        rbY += rbH + 4;
    }

    // Reset hotkeys to defaults button
    HWND hResetBtn = CreateWindow(L"BUTTON", L"Reset to defaults",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        rbX_lbl, rbY + 4, 160, rbH, hwnd, (HMENU)ID_BTN_RESET_KEYS, NULL, NULL);
    SendMessage(hResetBtn, WM_SETFONT, (WPARAM)hSmall, TRUE);
    rbY += rbH + 12;

    // Mic capture checkbox
    HWND hMic = CreateWindow(L"BUTTON", L"Also capture microphone (off by default)",
        WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
        rbX_lbl, rbY, 250, rbH, hwnd, (HMENU)ID_CHK_MIC, NULL, NULL);
    SendMessage(hMic, WM_SETFONT, (WPARAM)hSmall, TRUE);
    SendMessage(hMic, BM_SETCHECK, s_config->capture_mic ? BST_CHECKED : BST_UNCHECKED, 0);
    rbY += rbH + 8;

    // Output (loopback) device picker
    HWND hOutLbl = CreateWindow(L"STATIC", L"Output device (for system audio):",
        WS_VISIBLE | WS_CHILD,
        rbX_lbl, rbY, 250, 18, hwnd, NULL, NULL, NULL);
    SendMessage(hOutLbl, WM_SETFONT, (WPARAM)hSmall, TRUE);
    rbY += 20;

    HWND hOutDev = CreateWindow(L"COMBOBOX", NULL,
        WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        rbX_lbl, rbY, 240, 200, hwnd, (HMENU)ID_COMBO_DEV_OUT, NULL, NULL);
    SendMessage(hOutDev, WM_SETFONT, (WPARAM)hSmall, TRUE);
    s_outDevices = AudioCapture::EnumerateDevices(true);
    SendMessageW(hOutDev, CB_ADDSTRING, 0, (LPARAM)L"(default output device)");
    int outSel = 0;
    for (size_t i = 0; i < s_outDevices.size(); ++i) {
        SendMessageW(hOutDev, CB_ADDSTRING, 0, (LPARAM)Utf8To16(s_outDevices[i].name).c_str());
        if (s_outDevices[i].id == s_config->audio_device_id) outSel = (int)(i + 1);
    }
    SendMessage(hOutDev, CB_SETCURSEL, outSel, 0);
    rbY += rbH + 6;

    // Mic device picker
    HWND hMicLbl = CreateWindow(L"STATIC", L"Microphone device:",
        WS_VISIBLE | WS_CHILD,
        rbX_lbl, rbY, 250, 18, hwnd, NULL, NULL, NULL);
    SendMessage(hMicLbl, WM_SETFONT, (WPARAM)hSmall, TRUE);
    rbY += 20;

    HWND hMicDev = CreateWindow(L"COMBOBOX", NULL,
        WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        rbX_lbl, rbY, 240, 200, hwnd, (HMENU)ID_COMBO_DEV_MIC, NULL, NULL);
    SendMessage(hMicDev, WM_SETFONT, (WPARAM)hSmall, TRUE);
    s_micDevices = AudioCapture::EnumerateDevices(false);
    SendMessageW(hMicDev, CB_ADDSTRING, 0, (LPARAM)L"(default mic)");
    int micSel = 0;
    for (size_t i = 0; i < s_micDevices.size(); ++i) {
        SendMessageW(hMicDev, CB_ADDSTRING, 0, (LPARAM)Utf8To16(s_micDevices[i].name).c_str());
        if (s_micDevices[i].id == s_config->mic_device_id) micSel = (int)(i + 1);
    }
    SendMessage(hMicDev, CB_SETCURSEL, micSel, 0);
    rbY += rbH + 8;

    // Sound-on-auto checkbox
    HWND hSnd = CreateWindow(L"BUTTON", L"Beep when auto-answer fires",
        WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
        rbX_lbl, rbY, 250, rbH, hwnd, (HMENU)ID_CHK_SOUND_AUTO, NULL, NULL);
    SendMessage(hSnd, WM_SETFONT, (WPARAM)hSmall, TRUE);
    SendMessage(hSnd, BM_SETCHECK, s_config->sound_on_auto ? BST_CHECKED : BST_UNCHECKED, 0);

    OnProviderChanged(hwnd);

    // ---- Tooltips on the major controls ----
    INITCOMMONCONTROLSEX iccx{ sizeof(iccx), ICC_BAR_CLASSES };
    InitCommonControlsEx(&iccx);
    HWND hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, NULL,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hwnd, NULL, NULL, NULL);
    if (hTip) {
        SetWindowPos(hTip, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        auto addTip = [&](int ctlId, LPCWSTR text) {
            HWND ctl = GetDlgItem(hwnd, ctlId);
            if (!ctl) return;
            TOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
            ti.hwnd = hwnd;
            ti.uId = (UINT_PTR)ctl;
            ti.lpszText = (LPWSTR)text;
            SendMessageW(hTip, TTM_ADDTOOL, 0, (LPARAM)&ti);
            SendMessageW(hTip, TTM_SETMAXTIPWIDTH, 0, 300);
        };
        addTip(ID_COMBO_PROVIDER,  L"Which LLM service to use. Only Gemini accepts audio.");
        addTip(ID_COMBO_MODEL,     L"Model ID. Free-text — type any model name your provider supports.");
        addTip(ID_EDIT_APIKEY,     L"API key from your provider's console (Google AI Studio, OpenAI, etc.). Stored locally.");
        addTip(ID_BTN_CLEAR_KEY,   L"Erase the API key field.");
        addTip(ID_EDIT_BASEURL,    L"Only for Custom provider. e.g. http://localhost:11434/v1 for Ollama.");
        addTip(ID_EDIT_GEMINI_KEY, L"Optional — used when you pick a non-Gemini provider but still want F7/audio to work (routed via Gemini).");
        addTip(ID_EDIT_SESSION,    L"Conversation history is saved to chat.<this name>.txt. Use different names for different topics.");
        addTip(ID_CHK_MIC,         L"When checked, your microphone is also captured and mixed in. Off by default (interview use).");
        addTip(ID_CHK_SOUND_AUTO,  L"Play a brief beep when auto-answer mode fires an answer.");
        addTip(ID_COMBO_DEV_OUT,   L"Output device whose audio gets captured (system speakers normally).");
        addTip(ID_COMBO_DEV_MIC,   L"Mic device used when 'Also capture microphone' is on.");
        addTip(ID_BTN_RESET_KEYS,  L"Restore all rebindable hotkeys to their default bindings.");
        addTip(ID_BTN_START,       L"Save settings and launch the overlay.");
    }
}

std::string ConfigDialog::CurrentProviderId(HWND hwnd) {
    HWND hProv = GetDlgItem(hwnd, ID_COMBO_PROVIDER);
    int idx = (int)SendMessage(hProv, CB_GETCURSEL, 0, 0);
    const auto& providers = ConfigLoader::BuiltinProviders();
    if (idx < 0 || idx >= (int)providers.size()) return "gemini";
    return providers[idx].id;
}

void ConfigDialog::OnProviderChanged(HWND hwnd) {
    std::string provId = CurrentProviderId(hwnd);
    const ProviderInfo* prov = ConfigLoader::FindProvider(provId);
    if (!prov) return;

    HWND hModel = GetDlgItem(hwnd, ID_COMBO_MODEL);
    SendMessage(hModel, CB_RESETCONTENT, 0, 0);

    // Built-in defaults for this provider
    int selected = 0;
    for (size_t i = 0; i < prov->models.size(); ++i) {
        const auto& m = prov->models[i];
        std::string display = m.name.empty() ? m.id : (m.name + " (" + m.id + ")");
        SendMessageW(hModel, CB_ADDSTRING, 0, (LPARAM)Utf8To16(display).c_str());
        if (!s_config->model.empty() && m.id == s_config->model) selected = (int)i;
    }

    // For Gemini also append any user-provided models from models_list.txt
    if (provId == "gemini" && s_models) {
        for (const auto& m : *s_models) {
            // skip duplicates already in built-in list
            bool dup = false;
            for (const auto& bm : prov->models) {
                if (bm.id == m.id) { dup = true; break; }
            }
            if (dup) continue;
            std::string display = m.name.empty() ? m.id : (m.name + " (" + m.id + ")");
            SendMessageW(hModel, CB_ADDSTRING, 0, (LPARAM)Utf8To16(display).c_str());
            if (!s_config->model.empty() && m.id == s_config->model) {
                selected = (int)(prov->models.size() + (&m - &(*s_models)[0]));
            }
        }
    }

    SendMessage(hModel, CB_SETCURSEL, selected, 0);

    // Show base URL field only when Custom is selected
    HWND hLblBase = GetDlgItem(hwnd, ID_LBL_BASEURL);
    HWND hBase    = GetDlgItem(hwnd, ID_EDIT_BASEURL);
    int show = (provId == "custom") ? SW_SHOW : SW_HIDE;
    ShowWindow(hLblBase, show);
    ShowWindow(hBase, show);
}

void ConfigDialog::SaveAndClose(HWND hwnd) {
    if (!s_config) return;

    // Hotkey conflict check: any two non-empty bindings with the same vk+mods.
    for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
        const auto& a = s_config->hotkeys.bindings[i];
        if (a.empty()) continue;
        for (int j = i + 1; j < (int)HotkeyAction::Count; ++j) {
            const auto& b = s_config->hotkeys.bindings[j];
            if (b.empty()) continue;
            if (a.vk == b.vk && a.modifiers == b.modifiers) {
                std::wstring msg = L"Hotkey conflict:\n\n  ";
                msg += Utf8To16(ConfigLoader::ActionLabel((HotkeyAction)i));
                msg += L"\n  and\n  ";
                msg += Utf8To16(ConfigLoader::ActionLabel((HotkeyAction)j));
                msg += L"\n\nare both bound to '";
                msg += Utf8To16(ConfigLoader::BindingToString(a));
                msg += L"'. Pick a different key for one of them.";
                MessageBoxW(hwnd, msg.c_str(), L"Hotkey conflict", MB_ICONERROR);
                return;
            }
        }
    }

    // Provider
    s_config->provider = CurrentProviderId(hwnd);
    const ProviderInfo* prov = ConfigLoader::FindProvider(s_config->provider);

    // Model — read the combo text (free-text) so users can type custom model IDs.
    HWND hModel = GetDlgItem(hwnd, ID_COMBO_MODEL);
    char modelBuf[256] = {0};
    GetWindowTextA(hModel, modelBuf, sizeof(modelBuf));
    std::string raw = modelBuf;

    // If the user picked an entry like "Display Name (model-id)", extract model-id;
    // otherwise treat the entire string as the model id.
    std::string parsedId = raw;
    size_t lparen = raw.rfind('(');
    size_t rparen = raw.rfind(')');
    if (lparen != std::string::npos && rparen != std::string::npos && rparen > lparen) {
        parsedId = raw.substr(lparen + 1, rparen - lparen - 1);
    }
    s_config->model = parsedId;

    // API Key
    HWND hEdit = GetDlgItem(hwnd, ID_EDIT_APIKEY);
    int len = GetWindowTextLength(hEdit);
    std::vector<char> buf(len + 1);
    GetWindowTextA(hEdit, buf.data(), len + 1);
    s_config->api_key = buf.data();

    // Base URL (only relevant for custom)
    HWND hBase = GetDlgItem(hwnd, ID_EDIT_BASEURL);
    int len2 = GetWindowTextLength(hBase);
    std::vector<char> buf2(len2 + 1);
    GetWindowTextA(hBase, buf2.data(), len2 + 1);
    s_config->base_url = buf2.data();

    // Mic capture checkbox
    HWND hMic = GetDlgItem(hwnd, ID_CHK_MIC);
    s_config->capture_mic = (SendMessage(hMic, BM_GETCHECK, 0, 0) == BST_CHECKED);

    // Gemini fallback key
    HWND hGem = GetDlgItem(hwnd, ID_EDIT_GEMINI_KEY);
    int len3 = GetWindowTextLength(hGem);
    std::vector<char> buf3(len3 + 1);
    GetWindowTextA(hGem, buf3.data(), len3 + 1);
    s_config->gemini_fallback_key = buf3.data();

    // Output device — index 0 = "(default)"
    HWND hOutDev = GetDlgItem(hwnd, ID_COMBO_DEV_OUT);
    int outIdx = (int)SendMessage(hOutDev, CB_GETCURSEL, 0, 0);
    if (outIdx > 0 && outIdx - 1 < (int)s_outDevices.size()) {
        s_config->audio_device_id = s_outDevices[outIdx - 1].id;
    } else {
        s_config->audio_device_id.clear();
    }

    // Mic device — index 0 = "(default)"
    HWND hMicDev = GetDlgItem(hwnd, ID_COMBO_DEV_MIC);
    int micIdx = (int)SendMessage(hMicDev, CB_GETCURSEL, 0, 0);
    if (micIdx > 0 && micIdx - 1 < (int)s_micDevices.size()) {
        s_config->mic_device_id = s_micDevices[micIdx - 1].id;
    } else {
        s_config->mic_device_id.clear();
    }

    // Session name
    HWND hSess = GetDlgItem(hwnd, ID_EDIT_SESSION);
    int sessLen = GetWindowTextLength(hSess);
    std::vector<char> sessBuf(sessLen + 1);
    GetWindowTextA(hSess, sessBuf.data(), sessLen + 1);
    std::string sn = sessBuf.data();
    if (sn.empty()) sn = "default";
    s_config->session_name = sn;

    // Sound on auto
    HWND hSnd = GetDlgItem(hwnd, ID_CHK_SOUND_AUTO);
    s_config->sound_on_auto = (SendMessage(hSnd, BM_GETCHECK, 0, 0) == BST_CHECKED);

    // Validate
    if (s_config->api_key.empty() && s_config->provider != "custom") {
        MessageBox(hwnd, L"Please enter an API Key.", L"Error", MB_ICONERROR);
        return;
    }
    if (s_config->model.empty()) {
        MessageBox(hwnd, L"Please pick or type a model.", L"Error", MB_ICONERROR);
        return;
    }
    if (s_config->provider == "custom" && s_config->base_url.empty()) {
        MessageBox(hwnd, L"Please enter a Base URL for the custom provider.", L"Error", MB_ICONERROR);
        return;
    }

    s_success = true;
    DestroyWindow(hwnd);
}
