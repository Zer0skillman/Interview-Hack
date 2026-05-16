#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include "ConfigLoader.h"

class ConfigDialog {
public:
    static bool Show(HINSTANCE hInstance, LLMConfig& config, const std::vector<ModelInfo>& models);

    // Public so the rebind-button subclass proc (free function) can access it.
    static LLMConfig* s_config;

private:
    static LRESULT CALLBACK DialogProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static void InitializeControls(HWND hwnd);
    static void SaveAndClose(HWND hwnd);

    // Dialog Controls IDs
    static const int ID_COMBO_PROVIDER = 100;
    static const int ID_COMBO_MODEL    = 101;
    static const int ID_EDIT_APIKEY    = 102;
    static const int ID_BTN_START      = 103;
    static const int ID_EDIT_BASEURL   = 104;
    static const int ID_LBL_BASEURL    = 105;
    static const int ID_BTN_CLEAR_KEY    = 106;
    static const int ID_BTN_RESET_KEYS   = 107;
    static const int ID_CHK_MIC          = 108;
    static const int ID_EDIT_GEMINI_KEY  = 109;
    static const int ID_COMBO_DEV_OUT    = 110;
    static const int ID_COMBO_DEV_MIC    = 111;
    static const int ID_EDIT_SESSION     = 112;
    static const int ID_CHK_SOUND_AUTO   = 113;
    // Rebind buttons take IDs 200, 201, ... one per HotkeyAction
    static const int ID_REBIND_BASE    = 200;

    // State
    static const std::vector<ModelInfo>* s_models;  // user-provided models_list.txt (Gemini extras)
    static bool s_success;

    static void OnProviderChanged(HWND hwnd);
    static std::string CurrentProviderId(HWND hwnd);
};
