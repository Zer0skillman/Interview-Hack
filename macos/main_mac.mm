#import <Cocoa/Cocoa.h>

#include "../ConfigLoader.h"
#include "../Logger.h"
#include "../Updater.h"

#include "MacOverlayWindow.h"

#include <csignal>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// macOS-specific entry point. Mirrors the Win main.cpp shape:
//   - install a crash log handler
//   - Updater::CleanupAfterRestart (no-op on Mac)
//   - show the config dialog (welcome screen)
//   - build the overlay and run the main run loop
//
// LSUIElement=true in Info.plist keeps us out of the Dock and Cmd-Tab; the
// overlay window itself handles its own visibility.

namespace MacConfigDialog {
    bool Show(LLMConfig& config, const std::vector<ModelInfo>& models);
}

static void CrashHandler(int sig) {
    std::error_code ec;
    std::filesystem::create_directory("logs", ec);
    FILE* f = std::fopen("logs/crash.txt", "wb");
    if (f) {
        std::fprintf(f, "Crash: signal %d\nVersion: 2.5.0 (macOS)\n", sig);
        std::fclose(f);
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv;

    // Catch the common crash signals so we leave a logs/crash.txt behind.
    std::signal(SIGSEGV, CrashHandler);
    std::signal(SIGABRT, CrashHandler);
    std::signal(SIGBUS,  CrashHandler);
    std::signal(SIGFPE,  CrashHandler);
    std::signal(SIGILL,  CrashHandler);

    Logger::Info("startup v2.5.0 (macOS)");
    Updater::CleanupAfterRestart();  // no-op on Mac, here for symmetry

    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        // Accessory: no Dock icon, no menu bar. The overlay still has full
        // input focus when interacted with.
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        // Load config + models the same way the Win build does
        LLMConfig config = ConfigLoader::LoadConfig("llm_config.txt");
        std::vector<ModelInfo> models = ConfigLoader::LoadModels("models_list.txt");

        if (!MacConfigDialog::Show(config, models)) {
            Logger::Info("welcome dialog cancelled — exiting");
            return 0;
        }

        ConfigLoader::SaveConfig("llm_config.txt", config);

        MacOverlayWindow overlay;
        overlay.SetConfig(config);

        if (!overlay.Initialize()) {
            NSAlert* a = [[NSAlert alloc] init];
            [a setMessageText:@"Failed to initialize overlay window."];
            [a runModal];
            return 1;
        }

        overlay.RunMessageLoop();
    }

    return 0;
}
