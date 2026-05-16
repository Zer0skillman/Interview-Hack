#pragma once
#ifdef __APPLE__

#include "../ConfigLoader.h"
#include "../IAudioCapture.h"
#include "../IScreenshot.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

// Macros so the Windows-style ChatMessage struct compiles on Mac too. We
// don't include OverlayWindow.h here because that drags in <windows.h>.
struct ChatMessage {
    std::wstring text;
    bool isUser = false;
    int  hour = -1;
    int  minute = -1;
};

// Pimpl: keeps the Objective-C / AppKit types out of this header so the rest
// of the C++ codebase can include it without having to be compiled as .mm.
class MacOverlayWindowImpl;

class MacOverlayWindow {
public:
    MacOverlayWindow();
    ~MacOverlayWindow();

    bool Initialize();
    void RunMessageLoop();
    void SetConfig(const LLMConfig& cfg);

    // Operations driven from hotkeys / NSTimer / dispatch_async.
    void CaptureScreenOnly();
    void CaptureAudioOnly();
    void UpdateFromClipboard();
    void ResetConversation();
    void CopyLastAnswer();
    void ToggleVisibility();
    void ToggleAutoMode();
    void ToggleMoveMode();
    void ToggleSelectMode();
    void ExitApp();
    void ScrollChat(int delta);

private:
    std::unique_ptr<MacOverlayWindowImpl> m_impl;
};

#endif  // __APPLE__
