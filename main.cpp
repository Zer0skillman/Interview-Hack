#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include "OverlayWindow.h"
#include "ConfigLoader.h"
#include "ConfigDialog.h"
#include "Logger.h"
#include "Updater.h"

#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.h")

// Unhandled-exception filter — writes a brief crash record so the user can
// report the failure mode. We don't dump symbols (no dbghelp dep) — just the
// exception code and faulting address.
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    CreateDirectoryW(L"logs", NULL);
    HANDLE h = CreateFileW(L"logs\\crash.txt", GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st; GetLocalTime(&st);
        char buf[512];
        int n = std::snprintf(buf, sizeof(buf),
            "Crash at %04d-%02d-%02d %02d:%02d:%02d\n"
            "Exception code: 0x%08lX\n"
            "Faulting address: %p\n"
            "Version: 2.5.0\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            (unsigned long)ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress);
        DWORD wrote;
        WriteFile(h, buf, (DWORD)n, &wrote, NULL);
        CloseHandle(h);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    SetUnhandledExceptionFilter(CrashFilter);
    Logger::Info("startup v2.5.0");

    // If a previous run swapped us in via the updater, clean up the .old file
    Updater::CleanupAfterRestart();

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    // GDI+ is used to encode screenshots as PNG for the multimodal LLM call.
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    LLMConfig config = ConfigLoader::LoadConfig("llm_config.txt");
    std::vector<ModelInfo> models = ConfigLoader::LoadModels("models_list.txt");

    if (!ConfigDialog::Show(hInstance, config, models)) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
        return 0;
    }

    ConfigLoader::SaveConfig("llm_config.txt", config);

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

    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
