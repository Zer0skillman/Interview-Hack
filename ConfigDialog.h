#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include "ConfigLoader.h"

class ConfigDialog {
public:
    static bool Show(HINSTANCE hInstance, LLMConfig& config, const std::vector<ModelInfo>& models);

private:
    static LRESULT CALLBACK DialogProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static void InitializeControls(HWND hwnd);
    static void SaveAndClose(HWND hwnd);

    // Dialog Controls IDs
    static const int ID_COMBO_MODEL = 101;
    static const int ID_EDIT_APIKEY = 102;
    static const int ID_BTN_START = 103;

    // State
    static LLMConfig* s_config;
    static const std::vector<ModelInfo>* s_models;
    static bool s_success;
};
