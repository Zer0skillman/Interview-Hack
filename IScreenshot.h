#pragma once
#include <string>
#include <memory>

// Captures a screenshot of the user's display(s) and returns it base64-encoded.
//
// Implementations:
//   - WindowsScreenshot (Screenshot_Win.cpp) — GDI BitBlt + GDI+ PNG encoder
//   - MacScreenshot     (forthcoming, macos/Screenshot_Mac.mm) — CGWindowListCreateImage
//
// Same factory pattern as IAudioCapture.
class IScreenshot {
public:
    virtual ~IScreenshot() = default;

    // Capture the monitor under the cursor, encode as PNG, return base64.
    // Empty string on failure. The capturing app's own window must not appear
    // in the resulting image (Windows uses WDA_EXCLUDEFROMCAPTURE; macOS uses
    // NSWindowSharingNone).
    virtual std::string CaptureMonitorUnderCursorAsBase64Png() = 0;
};

std::unique_ptr<IScreenshot> CreateScreenshot();
