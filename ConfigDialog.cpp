#include "ConfigDialog.h"
#include <commctrl.h>

LLMConfig* ConfigDialog::s_config = nullptr;
const std::vector<ModelInfo>* ConfigDialog::s_models = nullptr;
bool ConfigDialog::s_success = false;

bool ConfigDialog::Show(HINSTANCE hInstance, LLMConfig& config, const std::vector<ModelInfo>& models) {
    s_config = &config;
    s_models = &models;
    s_success = false;

    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = DialogProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wc.lpszClassName = L"ConfigDialogClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);

    // Check if class already exists to avoid error on subsequent opens
    WNDCLASSEX wcExisting;
    if (!GetClassInfoEx(hInstance, wc.lpszClassName, &wcExisting)) {
        RegisterClass(&wc);
    }

    // Center on screen
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int winW = 400;
    int winH = 250;
    int x = (screenW - winW) / 2;
    int y = (screenH - winH) / 2;

    HWND hwnd = CreateWindow(wc.lpszClassName, L"LLM Setup", 
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, winW, winH, 
        NULL, NULL, hInstance, NULL);

    if (!hwnd) return false;

    // Message Loop for Dialog
    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // GetMessage returns 0 on WM_QUIT. 
    // wParam contains the exit code passed to PostQuitMessage.
    if (msg.message == WM_QUIT && msg.wParam == 1) {
        return true;
    }
    
    return false;
}

LRESULT CALLBACK ConfigDialog::DialogProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE:
        InitializeControls(hwnd);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_BTN_START) {
            SaveAndClose(hwnd);
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd); // Trigger WM_DESTROY
        return 0;

    case WM_DESTROY:
        PostQuitMessage(s_success ? 1 : 0);
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void ConfigDialog::InitializeControls(HWND hwnd) {
    if (!s_config || !s_models) return;

    // Font
    HFONT hFont = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, 
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    // Labels
    HWND hLbl1 = CreateWindow(L"STATIC", L"Select Model:", WS_VISIBLE | WS_CHILD, 20, 20, 100, 20, hwnd, NULL, NULL, NULL);
    SendMessage(hLbl1, WM_SETFONT, (WPARAM)hFont, TRUE);

    HWND hLbl2 = CreateWindow(L"STATIC", L"API Key:", WS_VISIBLE | WS_CHILD, 20, 80, 100, 20, hwnd, NULL, NULL, NULL);
    SendMessage(hLbl2, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Combo Box (Model)
    HWND hCombo = CreateWindow(L"COMBOBOX", NULL, 
        WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        20, 45, 340, 200, hwnd, (HMENU)ID_COMBO_MODEL, NULL, NULL);
    SendMessage(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Populate Combo
    int selectedIndex = 0;
    for (size_t i = 0; i < s_models->size(); ++i) {
        const auto& info = (*s_models)[i];
        
        // Format: "Name (ID)" for clarity
        std::string displayText = info.name + " (" + info.id + ")";
        std::wstring wText(displayText.begin(), displayText.end());
        
        SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)wText.c_str());

        if (info.id == s_config->model) {
            selectedIndex = (int)i;
        }
    }

    SendMessage(hCombo, CB_SETCURSEL, selectedIndex, 0);

    // Edit (API Key)
    HWND hEdit = CreateWindow(L"EDIT", NULL, 
        WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 
        20, 105, 340, 25, hwnd, (HMENU)ID_EDIT_APIKEY, NULL, NULL);
    SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    // Set current key
    SetWindowTextA(hEdit, s_config->api_key.c_str());

    // Button (Start)
    HWND hBtn = CreateWindow(L"BUTTON", L"Start Overlay", 
        WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        130, 160, 120, 35, hwnd, (HMENU)ID_BTN_START, NULL, NULL);
    SendMessage(hBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
}

void ConfigDialog::SaveAndClose(HWND hwnd) {
    if (!s_config) return;

    // Get Model
    HWND hCombo = GetDlgItem(hwnd, ID_COMBO_MODEL);
    int index = SendMessage(hCombo, CB_GETCURSEL, 0, 0);
    if (index != CB_ERR && index >= 0 && index < (int)s_models->size()) {
        s_config->model = (*s_models)[index].id; // Save ID, not name
    }

    // Get API Key
    HWND hEdit = GetDlgItem(hwnd, ID_EDIT_APIKEY);
    int len = GetWindowTextLength(hEdit);
    std::vector<char> buf(len + 1);
    GetWindowTextA(hEdit, buf.data(), len + 1);
    s_config->api_key = buf.data();

    // Validate
    if (s_config->api_key.empty()) {
        MessageBox(hwnd, L"Please enter an API Key.", L"Error", MB_ICONERROR);
        return;
    }

    // Success
    s_success = true;
    DestroyWindow(hwnd); 
}
