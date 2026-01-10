#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "OverlayWindow.h"

#include "ConfigLoader.h"
#include "ConfigDialog.h"

#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

// Global Process Info for Backend
PROCESS_INFORMATION g_piBackend = { 0 };

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

void StartBackend(const LLMConfig& config) {
    // 1. Set Env Vars for the Child Process (Inherited)
    SetEnvironmentVariable(L"AI_PROVIDER", Utf8ToWide(config.provider).c_str());
    SetEnvironmentVariable(L"AI_MODEL", Utf8ToWide(config.model).c_str());
    SetEnvironmentVariable(L"AI_API_KEY", Utf8ToWide(config.api_key).c_str());
    SetEnvironmentVariable(L"HOST", L"127.0.0.1");
    SetEnvironmentVariable(L"PORT", L"8000");

    // 2. Launch ai_backend.exe
    STARTUPINFO si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // Hide console

    ZeroMemory(&g_piBackend, sizeof(g_piBackend));

    // Assume ai_backend.exe is in the same folder
    wchar_t cmd[] = L"ai_backend.exe"; 
    
    if (!CreateProcess(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &g_piBackend)) {
        // Only warn, don't crash. Maybe user hasn't built it yet or wants to run it manually.
        // MessageBox(NULL, L"Could not start ai_backend.exe. Please ensure it is in the same folder.", L"Warning", MB_ICONWARNING);
        OutputDebugString(L"Failed to start backend process.\n");
    } else {
         // Wait a moment for backend to spin up
        Sleep(2000);
    }
}

void StopBackend() {
    if (g_piBackend.hProcess) {
        TerminateProcess(g_piBackend.hProcess, 0);
        CloseHandle(g_piBackend.hProcess);
        CloseHandle(g_piBackend.hThread);
        ZeroMemory(&g_piBackend, sizeof(g_piBackend));
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    // Initialize Common Controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    // 1. Load Data
    LLMConfig config = ConfigLoader::LoadConfig("llm_config.txt");
    std::vector<ModelInfo> models = ConfigLoader::LoadModels("models_list.txt");

    // 2. Show Setup Dialog
    if (!ConfigDialog::Show(hInstance, config, models)) {
        return 0; // Cancelled
    }

    // 3. Save Selection
    ConfigLoader::SaveConfig("llm_config.txt", config);

    // 4. Start Backend
    StartBackend(config);

    // 5. Start Overlay
    OverlayWindow overlay;
    overlay.SetConfig(config); 

    if (overlay.Initialize(hInstance))
    {
        overlay.RunMessageLoop();
    }
    else
    {
        MessageBox(NULL, L"Failed to initialize overlay window.", L"Error", MB_ICONERROR);
    }

    // 6. Cleanup
    StopBackend();

    return 0;
}

