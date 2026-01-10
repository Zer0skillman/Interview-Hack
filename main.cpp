#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "OverlayWindow.h"

#include "ConfigLoader.h"
#include "ConfigDialog.h"

#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    // Initialize Common Controls (Best Practice)
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

    // 4. Start Overlay
    OverlayWindow overlay;
    
    // We pass the config to the overlay
    overlay.SetConfig(config); 

    if (overlay.Initialize(hInstance))
    {
        // Debug: Confirm we reached this point
        // MessageBox(NULL, L"Overlay Initialized! Starting Loop...", L"Debug", MB_OK); // Uncomment if needed
        overlay.RunMessageLoop();
    }
    else
    {
        MessageBox(NULL, L"Failed to initialize overlay window.", L"Error", MB_ICONERROR);
    }

    return 0;
}

