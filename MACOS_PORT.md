# macOS port — phase-2 handoff

You're picking up an in-progress port. The Windows version (`v2.4.4`) is
complete and shipping. Phase 1 (cross-platform scaffolding) is done. You're
writing phase 2 — actual Cocoa/CoreAudio/CoreGraphics code.

This doc tells you everything you need to know. Read it once before touching
any file.

---

## What you can assume already works

- **CMake build** (`CMakeLists.txt`) — knows about `if(APPLE)`, currently
  `FATAL_ERROR`s there. You flip that branch.
- **`IAudioCapture` interface** (`IAudioCapture.h`) — Windows impl works.
  You write `macos/MacAudioCapture.mm` implementing the same interface and
  a factory.
- **`IScreenshot` interface** (`IScreenshot.h`) — Windows impl works. Same
  deal for `macos/Screenshot_Mac.mm`.
- **Cross-platform business logic** in `LLMClient.cpp`, `ConfigLoader.cpp`,
  `Logger.cpp`, `Updater.cpp`, `Parsers.h` — these should compile as-is on
  macOS (they use `std::*`, file I/O via `std::fstream`, basic threading).
  Only `LLMClient.cpp` uses WinHTTP and will need work; see HTTP section below.
- **Tests** (`tests.cpp`) — pure C++, should compile and pass on macOS unchanged.

## What's genuinely platform-specific and needs you

| Subsystem | File you create | Replaces / parallels |
|---|---|---|
| Audio capture | `macos/MacAudioCapture.mm` | `AudioCapture.cpp` (WASAPI) |
| Screenshot | `macos/Screenshot_Mac.mm` | `Screenshot_Win.cpp` (GDI+) |
| Window + hotkeys + message loop | `macos/MacOverlayWindow.mm` | `OverlayWindow.cpp` (Win32) |
| Chat rendering | `macos/MacRenderer.mm` | `Overlay_Rendering.cpp` (GDI) |
| Welcome / settings dialog | `macos/MacConfigDialog.mm` | `ConfigDialog.cpp` (Win32 controls) |
| App entry point | `macos/main_mac.mm` | `main.cpp` (WinMain) |
| HTTP | `macos/MacHttp.mm` OR vcpkg libcurl | LLMClient/Updater WinHTTP code |
| App bundle resources | `macos/Info.plist`, app icon `.icns` | `app.rc` |

## Project layout after you start

```
core/         ← (optional) move cross-platform files here if you want
              ←  the directory shape obvious. Not required — CMake can pick
              ←  files from anywhere.
windows/      ← (optional) move .cpp files here
macos/        ← all your new code
  MacAudioCapture.mm
  MacAudioCapture.h
  Screenshot_Mac.mm
  MacOverlayWindow.mm
  MacOverlayWindow.h
  MacRenderer.mm
  MacConfigDialog.mm
  main_mac.mm
  Info.plist
  app.icns
```

You don't have to do the move. Mixed-flat is fine; CMake handles it. Cleaner
is to move during this phase since you're already touching everything.

---

## File-by-file implementation notes

### `macos/MacAudioCapture.mm` — implements `IAudioCapture`

The Windows version captures **system audio** (what other people say in your
Zoom meeting) via WASAPI loopback. This is the magic of the whole app.
**macOS doesn't expose system audio capture by default.** Three real options:

1. **ScreenCaptureKit** (macOS 13+) — modern, Apple-blessed, captures display
   audio. Requires Screen Recording permission prompt. This is the right path
   if you can require macOS 13+.
2. **CoreAudio aggregate device** trick — complex, fragile.
3. **BlackHole** (https://github.com/ExistentialAudio/BlackHole) — user
   installs a free virtual audio driver, routes system audio through it,
   your app captures from it. Adds a manual setup step but works on older
   macOS.

Recommendation: ScreenCaptureKit if your audience is on recent macOS.
Document the BlackHole alternative for older users.

ScreenCaptureKit audio capture entry points:
- `SCStream` with `SCContentFilter`
- Set `streamConfiguration.capturesAudio = YES`
- Implement `SCStreamOutput stream:didOutputSampleBuffer:ofType:` for
  `SCStreamOutputTypeAudio`
- Convert the `CMSampleBufferRef` audio to PCM, downsample to 16 kHz mono
  matching the Windows impl's ring buffer behavior

Your `IAudioCapture` impl needs:
- `Start(withMic, loopbackDeviceId, micDeviceId)` — start the SCStream;
  if `withMic` is true also start a `AVCaptureSession` for the mic and mix
  into the same ring buffer
- `Stop()` — stop both
- `SnapshotAsBase64Wav(seconds)` — pull last N seconds from ring buffer,
  WAV-encode, base64-encode. WAV header construction copied from
  `AudioCapture.cpp` works as-is.
- `RecentEnergy(seconds)` — compute RMS over the last N seconds for VAD

Free function `EnumerateAudioDevices(bool flowIsRender)`:
- For render: walk `[[AVAudioSession sharedInstance] availableInputs]`...
  actually that's iOS. On macOS use `AudioObjectGetPropertyData` with
  `kAudioHardwarePropertyDevices` to list audio devices.
- For each, get the device ID (as UTF-8 string) and friendly name

Permissions:
- Screen Recording (for ScreenCaptureKit): user grants in System Settings.
  First call triggers the prompt. Add `NSScreenCaptureUsageDescription`
  to Info.plist.
- Microphone: same flow. `NSMicrophoneUsageDescription` in Info.plist.

### `macos/Screenshot_Mac.mm` — implements `IScreenshot`

Single method: `CaptureMonitorUnderCursorAsBase64Png()`.

Modern path: `SCScreenshotManager.captureImage(contentFilter:configuration:)`
(macOS 14+). Older: `CGWindowListCreateImage` (deprecated but works).

For the active monitor under the cursor:
- `NSEvent.mouseLocation` → screen with `[NSScreen screens]` filtering by
  `containsPoint:`
- Capture only that screen's bounds

PNG encoding: `NSBitmapImageRep representationUsingType:NSBitmapImageFileTypePNG`
gives you `NSData *`. Base64: `[data base64EncodedStringWithOptions:0]`.

Same `WDA_EXCLUDEFROMCAPTURE` story: your overlay must use
`NSWindow.sharingType = NSWindowSharingNone` so it doesn't appear in its
own screenshot.

### `macos/MacOverlayWindow.mm` — replaces `OverlayWindow.cpp`

The Win32 message-loop pattern doesn't translate. You're building an AppKit
window from scratch. Big differences:

- **Window setup:** `NSWindow` with `styleMask:NSWindowStyleMaskBorderless`,
  `backing:NSBackingStoreBuffered`, `defer:NO`. Set:
  - `setLevel:NSStatusWindowLevel` — keeps it above other windows
  - `setSharingType:NSWindowSharingNone` — invisible to screen capture
  - `setIgnoresMouseEvents:YES` — click-through (toggle off for move/select mode)
  - `setBackgroundColor:[NSColor clearColor]` + `setOpaque:NO` for layered look
  - `setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
    NSWindowCollectionBehaviorStationary`

- **Hotkeys:** Carbon `RegisterEventHotKey` (Carbon is deprecated but the
  hotkey API still works fine — there's no AppKit replacement). Register
  same actions as Windows side, with the same Ctrl+Alt+* defaults.
  See `ConfigLoader::DefaultHotkeys()` for the mapping.

- **Update-check / polling timers:** `NSTimer` instead of `SetTimer`.

- **Threading:** still `std::thread` for the audio worker, but UI updates
  must dispatch back to main via `dispatch_async(dispatch_get_main_queue(), ...)`
  instead of `PostMessage`.

The behaviors to preserve are exhaustively documented in `OverlayWindow.cpp`
— read it once, then implement the macOS analogue. Look at:
- Send paths: `UpdateFromClipboard`, `CaptureScreenOnly`, `CaptureAudioOnly`
- Mode toggles: `ToggleMoveMode`, `ToggleSelectMode`, `ToggleAutoMode`,
  `ToggleSearch`, `ToggleHotkeyHints`
- Polling: `FirePoll`, `HandlePollResult`
- Stream chunk handler: `HandleLLMChunk`
- Persistence: `SaveConversation`, `LoadConversation`, `ExportChat`

The state (m_messages, m_config, m_audio, m_screenshot etc.) all moves over
the same way.

### `macos/MacRenderer.mm` — replaces `Overlay_Rendering.cpp`

The biggest file by lines (~830) and the most thankless port. The Windows
version uses GDI exclusively — `CreateFont`, `DrawText`, `RoundRect`,
`TextOut`, `BitBlt`, `FillRect`. None of this maps 1:1 to AppKit.

You have two strategies:

1. **`NSView drawRect:`** — implement the whole render in Cocoa drawing
   (`NSBezierPath`, `NSString drawAtPoint:withAttributes:`, etc.). Closest
   to the existing structure.
2. **`CALayer`-based** — composite layers per bubble. More complex setup,
   maybe nicer animation later.

Pick #1 unless you have a reason for #2.

Mapping table:
| GDI thing | AppKit equivalent |
|---|---|
| `CreateFont(...)` | `[NSFont fontWithName:size:]`, `+systemFontOfSize:` |
| `DrawText(..., DT_WORDBREAK)` | `[NSString drawInRect:withAttributes:]` (auto-wraps) |
| `TextOut` (positioned) | `[NSString drawAtPoint:withAttributes:]` |
| `RoundRect` | `[NSBezierPath bezierPathWithRoundedRect:xRadius:yRadius:]` then `fill` |
| `FillRect` + `HBRUSH` | `[NSColor set]` then `NSRectFill(rect)` |
| `BitBlt` | not needed for rendering; for screenshot see other file |
| `CreatePen` / `MoveToEx` / `LineTo` | `NSBezierPath` `moveToPoint:` `lineToPoint:` `stroke` |
| `SelectClipRgn` | `NSGraphicsContext.saveGraphicsState` + `NSBezierPath setClip` |

The OnPaint structure in `Overlay_Rendering.cpp` (measure pass + draw pass)
maps cleanly to AppKit. Port it function by function.

`PaintSeg`, `ParseSegments`, `ColorizeBrackets`, `ParseMdRuns`, `LayoutMd`,
`HasInlineMd`, `StripInlineMd`, `IsKeyword`, `IsKeywordInLang`, `InSet`,
`SplitLines` are all pure C++ string manipulation. **Copy them as-is** —
they work fine in an `.mm` file. Same for the color constants.

The chat-bubble layout math is in pixels everywhere — beware Retina. On
macOS use points and let the system handle the 2x backing store. Your
existing measure pass should mostly work; just don't multiply anything by
the backing scale factor.

### `macos/MacConfigDialog.mm` — replaces `ConfigDialog.cpp`

Welcome screen and settings UI. Win32 builds it imperatively with
`CreateWindow(L"COMBOBOX", ...)`. AppKit does the same kind of imperative
build with `NSPopUpButton`, `NSTextField`, `NSButton`, `NSWindow`.

Don't try to use Interface Builder (.xib files) — keeps the build simple
to do everything programmatically.

Layout: the current Win32 dialog is 700x760 with a left-column form and
right-column hotkey rebinder. Use the same proportions in AppKit. Look
at `ConfigDialog::InitializeControls` for the layout sequence.

Components to recreate:
- App brand header (`✨ Invisible AI Overlay` + tagline + accent line)
  — use `NSTextField` with custom attributed strings; or a custom NSView
- Provider dropdown (`NSPopUpButton`) — feeds from `ConfigLoader::BuiltinProviders()`
- Model combo (`NSPopUpButton` or `NSComboBox` for free text)
- API Key field (`NSSecureTextField` for password masking) + Clear button
- Base URL field (visible only when "Custom" provider selected)
- Gemini fallback key field (`NSSecureTextField`)
- Session combo (`NSComboBox`) + Delete button — enumerate `chat.*.txt`
  files in working dir
- Rebind buttons — 12 actions (HotkeyAction::Count). Each is a button
  that when clicked enters "press a key" capture mode. Use
  `NSEvent monitorMatchingMask:NSEventMaskKeyDown handler:` to capture
  the next keystroke + modifiers
- "Reset to defaults" button
- Mic capture checkbox + output/mic device combos (populated via
  `EnumerateAudioDevices(true/false)`)
- Sound-on-auto checkbox
- Start Overlay button (primary, prominent)

Hotkey conflict detection (same as Windows side): scan all bindings
on Save, reject duplicates with a clear message.

### `macos/main_mac.mm` — entry point

Replace `WinMain`. Use:

```objc
int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        // Crash filter equivalent: NSExceptionHandler or signal handlers
        // Updater::CleanupAfterRestart() — same as Windows main.cpp
        // Logger::Info("startup vX.Y.Z")

        // Show ConfigDialog modally (MacConfigDialog), bail on cancel
        // Construct OverlayWindow analogue (MacOverlayWindow)
        // Run NSApplication run loop

        [app run];
    }
    return 0;
}
```

### HTTP — two options

Current Windows code uses WinHTTP everywhere (LLMClient.cpp + Updater.cpp +
OverlayWindow.cpp). On macOS your choices:

**Option A: NSURLSession wrapped in C++**

Write `macos/MacHttp.mm` with an `IHttp`-shaped interface (Phase 1
deferred this abstraction — define it now if you go this route). Use
`NSURLSession dataTaskWithRequest:` for one-shot and
`NSURLSession streamTaskWithHostName:port:` for streaming.

Pros: native, no dep, handles HTTPS+redirects+SSL cleanly.
Cons: now you have two parallel HTTP impls (WinHTTP + NSURLSession) and a
new abstraction to maintain.

**Option B: libcurl via vcpkg, cross-platform**

Replace WinHTTP with libcurl everywhere. CMake picks up libcurl via
`find_package(CURL REQUIRED)` after `vcpkg install curl[ssl]`.

Pros: one HTTP impl across both platforms. Same SSE streaming code.
Cons: bigger commit, libcurl is a dependency, slightly heavier binary
on Windows.

Recommendation: **A for the first iteration** (less invasive, easier to
get Mac building). Migrate to **B** later if you want code unification.

### `macos/Info.plist`

Minimum keys:

```xml
<key>CFBundleName</key>                    <string>Invisible AI Overlay</string>
<key>CFBundleExecutable</key>              <string>overlay</string>
<key>CFBundleIdentifier</key>              <string>com.zer0skillman.interview-hack</string>
<key>CFBundleShortVersionString</key>      <string>2.4.4</string>
<key>NSScreenCaptureUsageDescription</key> <string>Captures meeting audio + screenshots so the AI can answer interview questions.</string>
<key>NSMicrophoneUsageDescription</key>    <string>Optional: also captures your microphone if you enable it in settings.</string>
<key>LSUIElement</key>                     <true/>  <!-- hide from Dock; overlay-only -->
```

### `macos/app.icns`

The Windows .ico (`app.ico`) is generated from a small PowerShell script.
For .icns you can use the same source PNG (regenerate with `iconutil` on
Mac) or commission a real icon. The current one is "AI" on a blue circle.

---

## CMake changes you'll make

`CMakeLists.txt` currently has:

```cmake
elseif(APPLE)
    message(FATAL_ERROR "macOS build target not yet implemented. ...")
```

Replace with something like:

```cmake
elseif(APPLE)
    enable_language(OBJCXX)
    set(MAC_SOURCES
        macos/main_mac.mm
        macos/MacAudioCapture.mm
        macos/Screenshot_Mac.mm
        macos/MacOverlayWindow.mm
        macos/MacRenderer.mm
        macos/MacConfigDialog.mm
        # macos/MacHttp.mm  ← only if going Option A
    )
    add_executable(overlay MACOSX_BUNDLE
        ${COMMON_SOURCES} ${COMMON_HEADERS} ${MAC_SOURCES})
    set_target_properties(overlay PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/macos/Info.plist"
        OUTPUT_NAME "Invisible AI Overlay"
    )
    target_link_libraries(overlay PRIVATE
        "-framework Cocoa"
        "-framework AppKit"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework ScreenCaptureKit"
        "-framework CoreGraphics"
        "-framework Carbon"        # for hotkeys
    )
    # If using libcurl (Option B):
    # find_package(CURL REQUIRED)
    # target_link_libraries(overlay PRIVATE CURL::libcurl)
endif()
```

## CI changes

`.github/workflows/build.yml` currently only runs windows-latest. Add a
second job for macos-latest once your macOS build works locally. Don't
push macOS code until it builds locally — CI iteration is slow.

## Versioning / release

The Windows release flow: tag `v*` → CI builds + zips `project_app/` →
publishes Release. Your macOS build can plug into the same workflow:
build the .app, codesign it (Apple Developer cert), notarize, zip it,
add to the same Release as `Interview-Hack-v*-macos.zip`.

For the first few iterations, just build locally and attach manually.

## Pitfalls

1. **Permission prompts.** First run of audio capture and screen capture
   each pops a system permission dialog. User has to grant in System
   Settings. The "invisible" pitch becomes slightly less invisible on
   first launch.
2. **NSWindowSharingNone** works against Zoom/Meet/Teams but **Apple's
   own QuickTime** and ScreenCaptureKit-based tools can sometimes still
   see your window. Test in the actual interview tools your users use.
3. **Gatekeeper / SmartScreen.** Unsigned `.app` bundles get a scary
   "developer not verified" prompt on first launch. Users have to
   right-click → Open. Apple Developer Program ($99/yr) + notarization
   fixes this. Worth doing before public release.
4. **Carbon RegisterEventHotKey** is deprecated for a decade but still
   works. Apple keeps it because there's no replacement. Don't be scared
   of the deprecation warning.
5. **ScreenCaptureKit needs macOS 13+.** If you need older support, use
   AVFoundation (`AVCaptureScreenInput`) — but that path is itself being
   deprecated. Pick a minimum OS version and document it.
6. **Retina coordinates.** Always work in points, not pixels. AppKit
   handles backing store scaling for you. Don't multiply anything by
   `backingScaleFactor` unless you actually need raw pixel buffers.

## How to verify each piece

1. Build the empty CMake APPLE branch first (just the COMMON_SOURCES) to
   make sure the cross-platform code compiles. Should fail at link due to
   missing `main`, but that's expected.
2. Add `macos/main_mac.mm` with a stub that opens a borderless NSWindow
   and runs. Verify you see something on screen.
3. Add Screenshot_Mac.mm. Wire a temporary hotkey to write a screenshot to
   `~/Desktop/test.png`. Confirm it works.
4. Add MacAudioCapture.mm with just a `Start` that hits Screen Recording
   permission. Confirm prompt appears, then user grants, then you can read
   audio samples.
5. Add MacRenderer.mm with one chat bubble. Verify text renders.
6. Build up from there incrementally — don't try to land all 5+ files
   in one diff.

---

## What to do first

```bash
git clone git@github.com:Zer0skillman/Interview-Hack.git
cd Interview-Hack
git log --oneline -5    # see where the project is

# Make sure Windows-side still builds (sanity check before changes)
# (skip if you don't have a Windows machine — just trust CI)

# Start a fresh Claude Code session on macOS in this directory.
# Claude will auto-load CLAUDE.md. Point it at this file:
#   "Read MACOS_PORT.md and start implementing phase 2"

# First commit aim: get the empty NSWindow showing.
# Don't try to do everything at once.
```

Good luck. The hard part is audio (ScreenCaptureKit's permission flow + the
PCM conversion). Everything else is mechanical translation.
