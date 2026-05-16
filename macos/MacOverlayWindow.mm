#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>

#include "MacOverlayWindow.h"
#include "../LLMClient.h"
#include "../Logger.h"
#include "../Updater.h"
#include "../ConfigLoader.h"
#include "../WinCompat.h"   // brings in VK_* / MOD_* defines on non-Windows

#include <atomic>
#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// MacOverlayWindow — Cocoa equivalent of OverlayWindow.cpp.
//
// Mirrors the core behavior set (capture screenshot, capture audio, send
// clipboard text, toggle visibility, exit) and streams the LLM response into
// the chat bubbles drawn by the NSView in MacRenderer.mm.
//
// The auto-poll, search, move-mode, and conversation-persistence features
// from the Windows side are present in scaffolding form but kept minimal —
// the goal of this first port is "works for live use" rather than 1:1 feature
// parity. Future commits flesh those out.

// -----------------------------------------------------------------------------
// Forward declarations into the renderer (MacRenderer.mm)
// -----------------------------------------------------------------------------

// The renderer (MacRenderer.mm) defines a private @interface ChatView : NSView.
// We treat it as an opaque NSView* on this side so we don't need to import the
// .mm.
extern "C" NSView* CreateChatView(NSRect frame);
extern "C" void ChatViewSetMessages(NSView* view, const std::vector<ChatMessage>* msgs);
extern "C" void ChatViewSetTranscript(NSView* view, NSString* text);
extern "C" void ChatViewSetTheme(NSView* view, const char* themeId);
extern "C" void ChatViewSetMode(NSView* view, BOOL autoMode, BOOL moveMode, BOOL selectMode);
extern "C" void ChatViewScroll(NSView* view, int delta);
extern "C" void ChatViewSetConfig(NSView* view, const LLMConfig* cfg);
extern "C" void ChatViewSetAudioLevel(NSView* view, float level);
extern "C" void ChatViewSetThinking(NSView* view, BOOL thinking);
extern "C" void ChatViewToggleHints(NSView* view);

// Re-show the welcome dialog so the user can rebind hotkeys / change provider
// without restarting the app. Implemented in MacConfigDialog.mm.
namespace MacConfigDialog {
    bool Show(LLMConfig& config, const std::vector<ModelInfo>& models);
}

// -----------------------------------------------------------------------------
// Carbon hotkey glue
// -----------------------------------------------------------------------------

// Mac modifier flags from Carbon
static UInt32 ConvertModifiers(unsigned int winMods) {
    UInt32 m = 0;
    if (winMods & MOD_CONTROL) m |= controlKey;
    if (winMods & MOD_ALT)     m |= optionKey;
    if (winMods & MOD_SHIFT)   m |= shiftKey;
    if (winMods & MOD_WIN)     m |= cmdKey;
    return m;
}

// Map Win-style VK_* to Carbon kVK_* keycodes. Letters and digits map by
// ASCII letter — Carbon keycodes are positional but we get the same effect
// for the limited subset we register (function keys + Ctrl+Alt+letter).
static UInt32 ConvertVk(unsigned int vk) {
    switch (vk) {
        case VK_F1:  return kVK_F1;
        case VK_F2:  return kVK_F2;
        case VK_F3:  return kVK_F3;
        case VK_F4:  return kVK_F4;
        case VK_F5:  return kVK_F5;
        case VK_F6:  return kVK_F6;
        case VK_F7:  return kVK_F7;
        case VK_F8:  return kVK_F8;
        case VK_F9:  return kVK_F9;
        case VK_F10: return kVK_F10;
        case VK_F11: return kVK_F11;
        case VK_F12: return kVK_F12;
        case VK_INSERT: return kVK_Help;   // No real Insert on Mac; Help key is the closest
        case VK_DELETE: return kVK_ForwardDelete;
        case VK_HOME:   return kVK_Home;
        case VK_END:    return kVK_End;
        case VK_PRIOR:  return kVK_PageUp;
        case VK_NEXT:   return kVK_PageDown;
        case VK_SPACE:  return kVK_Space;
        case VK_RETURN: return kVK_Return;
        case VK_ESCAPE: return kVK_Escape;
        default:
            // Letters & digits — Carbon has positional keycodes; use a switch.
            switch (vk) {
                case 'A': return kVK_ANSI_A; case 'B': return kVK_ANSI_B;
                case 'C': return kVK_ANSI_C; case 'D': return kVK_ANSI_D;
                case 'E': return kVK_ANSI_E; case 'F': return kVK_ANSI_F;
                case 'G': return kVK_ANSI_G; case 'H': return kVK_ANSI_H;
                case 'I': return kVK_ANSI_I; case 'J': return kVK_ANSI_J;
                case 'K': return kVK_ANSI_K; case 'L': return kVK_ANSI_L;
                case 'M': return kVK_ANSI_M; case 'N': return kVK_ANSI_N;
                case 'O': return kVK_ANSI_O; case 'P': return kVK_ANSI_P;
                case 'Q': return kVK_ANSI_Q; case 'R': return kVK_ANSI_R;
                case 'S': return kVK_ANSI_S; case 'T': return kVK_ANSI_T;
                case 'U': return kVK_ANSI_U; case 'V': return kVK_ANSI_V;
                case 'W': return kVK_ANSI_W; case 'X': return kVK_ANSI_X;
                case 'Y': return kVK_ANSI_Y; case 'Z': return kVK_ANSI_Z;
                case '0': return kVK_ANSI_0; case '1': return kVK_ANSI_1;
                case '2': return kVK_ANSI_2; case '3': return kVK_ANSI_3;
                case '4': return kVK_ANSI_4; case '5': return kVK_ANSI_5;
                case '6': return kVK_ANSI_6; case '7': return kVK_ANSI_7;
                case '8': return kVK_ANSI_8; case '9': return kVK_ANSI_9;
            }
            return 0xFFFF;  // sentinel; RegisterEventHotKey will reject
    }
}

// -----------------------------------------------------------------------------
// MacOverlayWindowImpl — the actual Cocoa-side state
// -----------------------------------------------------------------------------

class MacOverlayWindowImpl {
public:
    NSWindow* window = nil;
    NSView*   chatView = nil;
    NSTimer*  audioLevelTimer = nil;
    LLMConfig config;

    std::vector<ChatMessage> messages;
    std::mutex messagesMutex;

    std::unique_ptr<IAudioCapture> audio;
    std::unique_ptr<IScreenshot>   screenshot;

    std::atomic<int>  inflightCalls{0};
    std::atomic<bool> autoMode{false};
    std::atomic<bool> moveMode{false};
    std::atomic<bool> selectMode{false};

    std::vector<EventHotKeyRef> hotkeys;
    // Reserved IDs for hardcoded Mac-only hotkeys (hints/settings/about).
    // These are NOT part of the HotkeyAction enum (Windows F2 hints is also
    // hardcoded, not enum-driven).
    static constexpr UInt32 kHkToggleHints  = 9001;
    static constexpr UInt32 kHkOpenSettings = 9002;
    static constexpr UInt32 kHkShowAbout    = 9003;

    // Adds a new message in a thread-safe way and triggers a re-render on the
    // main thread.
    void AddMessage(const std::wstring& text, bool isUser) {
        std::time_t t = std::time(nullptr);
        std::tm tmv{};
        localtime_r(&t, &tmv);
        ChatMessage m;
        m.text = text;
        m.isUser = isUser;
        m.hour = tmv.tm_hour;
        m.minute = tmv.tm_min;
        {
            std::lock_guard<std::mutex> lk(messagesMutex);
            messages.push_back(m);
            // Trim to history_cap to bound memory.
            if ((int)messages.size() > config.history_cap) {
                messages.erase(messages.begin(),
                    messages.begin() + (messages.size() - config.history_cap));
            }
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            ChatViewSetMessages(chatView, &messages);
            [chatView setNeedsDisplay:YES];
        });
    }

    // Appends to the last message text (for streaming the bot reply).
    void AppendToLastBot(const std::wstring& chunk) {
        {
            std::lock_guard<std::mutex> lk(messagesMutex);
            if (messages.empty() || messages.back().isUser) {
                ChatMessage m;
                m.text = L"";
                m.isUser = false;
                std::time_t t = std::time(nullptr);
                std::tm tmv{};
                localtime_r(&t, &tmv);
                m.hour = tmv.tm_hour;
                m.minute = tmv.tm_min;
                messages.push_back(m);
            }
            messages.back().text += chunk;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            ChatViewSetMessages(chatView, &messages);
            [chatView setNeedsDisplay:YES];
        });
    }

    void Finalize() {
        dispatch_async(dispatch_get_main_queue(), ^{
            ChatViewSetMessages(chatView, &messages);
            [chatView setNeedsDisplay:YES];
        });
    }

    // Builds an LLMTurn history from the last messages, capped by config.
    std::vector<LLMTurn> History() {
        std::vector<LLMTurn> out;
        std::lock_guard<std::mutex> lk(messagesMutex);
        // Skip the very last user message (it's the current ask, sent separately)
        for (size_t i = 0; i + 1 < messages.size(); ++i) {
            LLMTurn t;
            t.isUser = messages[i].isUser;
            t.text = messages[i].text;
            out.push_back(t);
        }
        return out;
    }
};

// -----------------------------------------------------------------------------
// Carbon hotkey dispatch
// -----------------------------------------------------------------------------

namespace {
struct HotkeyEntry {
    UInt32 id;             // signature id; >=9001 means hardcoded special
    HotkeyAction action;   // valid when id < 9001
    MacOverlayWindow* owner;
};
static std::vector<HotkeyEntry> g_hotkeys;
static std::mutex g_hotkeyMutex;

static OSStatus HotkeyEventHandler(EventHandlerCallRef, EventRef event, void*) {
    EventHotKeyID hkid;
    OSStatus s = GetEventParameter(event, kEventParamDirectObject,
                                   typeEventHotKeyID, NULL,
                                   sizeof(hkid), NULL, &hkid);
    if (s != noErr) return s;

    HotkeyAction action = HotkeyAction::Count;
    MacOverlayWindow* owner = nullptr;
    UInt32 matchedId = 0;
    {
        std::lock_guard<std::mutex> lk(g_hotkeyMutex);
        for (auto& e : g_hotkeys) {
            if (e.id == hkid.id) { action = e.action; owner = e.owner; matchedId = e.id; break; }
        }
    }
    if (!owner) return noErr;

    dispatch_async(dispatch_get_main_queue(), ^{
        if (matchedId == MacOverlayWindowImpl::kHkToggleHints) {
            owner->ToggleHotkeyHints();
            return;
        }
        if (matchedId == MacOverlayWindowImpl::kHkOpenSettings) {
            owner->OpenRuntimeSettings();
            return;
        }
        if (matchedId == MacOverlayWindowImpl::kHkShowAbout) {
            owner->ShowAbout();
            return;
        }
        if (action == HotkeyAction::Count) return;
        switch (action) {
            case HotkeyAction::SendScreen:       owner->CaptureScreenOnly(); break;
            case HotkeyAction::SendAudio:        owner->CaptureAudioOnly(); break;
            case HotkeyAction::ToggleAuto:       owner->ToggleAutoMode(); break;
            case HotkeyAction::MoveMode:         owner->ToggleMoveMode(); break;
            case HotkeyAction::ResetChat:        owner->ResetConversation(); break;
            case HotkeyAction::CopyAnswer:       owner->CopyLastAnswer(); break;
            case HotkeyAction::SelectMode:       owner->ToggleSelectMode(); break;
            case HotkeyAction::SendText:         owner->UpdateFromClipboard(); break;
            case HotkeyAction::ToggleVisibility: owner->ToggleVisibility(); break;
            case HotkeyAction::ExitApp:          owner->ExitApp(); break;
            case HotkeyAction::ScrollUp:         owner->ScrollChat(-80); break;
            case HotkeyAction::ScrollDown:       owner->ScrollChat( 80); break;
            default: break;
        }
    });
    return noErr;
}

static void InstallHotkeyEventHandlerOnce() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    EventTypeSpec spec = { kEventClassKeyboard, kEventHotKeyPressed };
    InstallApplicationEventHandler(&HotkeyEventHandler, 1, &spec, NULL, NULL);
}

}  // namespace

// -----------------------------------------------------------------------------
// MacOverlayWindow public API
// -----------------------------------------------------------------------------

MacOverlayWindow::MacOverlayWindow()
    : m_impl(std::make_unique<MacOverlayWindowImpl>()) {}
MacOverlayWindow::~MacOverlayWindow() = default;

void MacOverlayWindow::SetConfig(const LLMConfig& cfg) {
    m_impl->config = cfg;
}

bool MacOverlayWindow::Initialize() {
    @autoreleasepool {
        // 1) Window setup — borderless, transparent, status-window-level, click-through.
        NSRect frame = NSMakeRect(40, 40, 520, 720);
        if (m_impl->config.win_w > 0 && m_impl->config.win_h > 0) {
            frame = NSMakeRect(m_impl->config.win_x, m_impl->config.win_y,
                               m_impl->config.win_w, m_impl->config.win_h);
        }

        m_impl->window = [[NSWindow alloc]
            initWithContentRect:frame
            styleMask:NSWindowStyleMaskBorderless
            backing:NSBackingStoreBuffered
            defer:NO];

        [m_impl->window setBackgroundColor:[NSColor clearColor]];
        [m_impl->window setOpaque:NO];
        [m_impl->window setLevel:NSStatusWindowLevel];
        // Hide from screen capture (the macOS analog of WDA_EXCLUDEFROMCAPTURE).
        [m_impl->window setSharingType:NSWindowSharingNone];
        [m_impl->window setIgnoresMouseEvents:YES];
        [m_impl->window setCollectionBehavior:
            NSWindowCollectionBehaviorCanJoinAllSpaces |
            NSWindowCollectionBehaviorStationary |
            NSWindowCollectionBehaviorFullScreenAuxiliary];
        [m_impl->window setReleasedWhenClosed:NO];

        // 2) The chat view fills the window.
        m_impl->chatView = CreateChatView([[m_impl->window contentView] bounds]);
        [m_impl->chatView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        ChatViewSetTheme(m_impl->chatView, m_impl->config.theme.c_str());
        [[m_impl->window contentView] addSubview:m_impl->chatView];

        [m_impl->window orderFront:nil];

        // 3) Audio + screenshot impls
        m_impl->audio = CreateAudioCapture();
        m_impl->screenshot = CreateScreenshot();
        m_impl->audio->Start(m_impl->config.capture_mic,
                             m_impl->config.audio_device_id,
                             m_impl->config.mic_device_id);

        // 4) Register hotkeys (Carbon)
        InstallHotkeyEventHandlerOnce();
        int registered = 0, failed = 0;
        for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
            HotkeyBinding b = m_impl->config.hotkeys.bindings[i];
            if (b.empty()) continue;
            UInt32 mods = ConvertModifiers(b.modifiers);
            UInt32 kc   = ConvertVk(b.vk);
            const char* actionId = ConfigLoader::ActionId((HotkeyAction)i);
            std::string label = ConfigLoader::BindingToString(b);

            if (kc == 0xFFFF) {
                Logger::Warn(std::string("hotkey skipped (no Mac keycode for VK=") +
                             std::to_string(b.vk) + "): " + actionId + " = " + label);
                failed++;
                continue;
            }
            EventHotKeyID hkid = { 'OVRL', (UInt32)(i + 1) };
            EventHotKeyRef ref = nullptr;
            OSStatus st = RegisterEventHotKey(kc, mods, hkid, GetApplicationEventTarget(),
                                              0, &ref);
            if (st == noErr) {
                m_impl->hotkeys.push_back(ref);
                {
                    std::lock_guard<std::mutex> lk(g_hotkeyMutex);
                    g_hotkeys.push_back({ (UInt32)(i + 1), (HotkeyAction)i, this });
                }
                Logger::Info(std::string("hotkey ") + label + " -> " + actionId);
                registered++;
            } else {
                // eventHotKeyExistsErr = -9878 — another app already owns this combo.
                Logger::Warn(std::string("hotkey FAILED (OSStatus ") + std::to_string(st) +
                             "): " + actionId + " = " + label);
                failed++;
            }
        }
        Logger::Info("hotkeys: " + std::to_string(registered) +
                     " registered, " + std::to_string(failed) + " failed");

        // Mac-only hardcoded extras (don't take a slot in HotkeyAction enum,
        // mirroring how F2/F1/F11 are hardcoded on Windows OverlayWindow.cpp).
        auto registerSpecial = [&](UInt32 id, UInt32 mods, UInt32 keyCode,
                                   const char* label) {
            EventHotKeyID hkid = { 'OVRL', id };
            EventHotKeyRef ref = nullptr;
            if (RegisterEventHotKey(keyCode, mods, hkid,
                                    GetApplicationEventTarget(), 0, &ref) == noErr) {
                m_impl->hotkeys.push_back(ref);
                std::lock_guard<std::mutex> lk(g_hotkeyMutex);
                g_hotkeys.push_back({ id, HotkeyAction::Count, this });
                Logger::Info(std::string("hotkey ") + label + " registered (special)");
            } else {
                Logger::Warn(std::string("hotkey ") + label + " FAILED (special)");
            }
        };
        // Cmd+Option+/ → toggle the hotkey hints panel
        registerSpecial(MacOverlayWindowImpl::kHkToggleHints,
                        cmdKey | optionKey, kVK_ANSI_Slash, "Cmd+Option+/ (hints)");
        // Cmd+Option+, → re-open welcome / settings dialog
        registerSpecial(MacOverlayWindowImpl::kHkOpenSettings,
                        cmdKey | optionKey, kVK_ANSI_Comma, "Cmd+Option+, (settings)");
        // Cmd+Option+I → about dialog
        registerSpecial(MacOverlayWindowImpl::kHkShowAbout,
                        cmdKey | optionKey, kVK_ANSI_I, "Cmd+Option+I (about)");

        // Hand the config to the renderer so it can render real hotkey strings
        // in the status bar / empty hint / hints panel.
        ChatViewSetConfig(m_impl->chatView, &m_impl->config);

        // Poll audio energy 6×/sec so the level dot in the status bar reflects
        // real-time meeting volume. NSTimer runs on the main run loop, which is
        // exactly where the renderer wants to be updated.
        __block MacOverlayWindowImpl* implPtr = m_impl.get();
        m_impl->audioLevelTimer = [NSTimer scheduledTimerWithTimeInterval:0.17
            repeats:YES
            block:^(NSTimer*) {
                float lvl = implPtr->audio ? implPtr->audio->RecentEnergy(1) : 0.0f;
                ChatViewSetAudioLevel(implPtr->chatView, lvl);
            }];

        // 5) Update check (non-blocking)
        if (!m_impl->config.update_check_url.empty()) {
            Updater::CheckAndDownloadAsync(
                m_impl->config.update_check_url,
                L"2.5.1",
                [this](const Updater::Status& st) {
                    if (st.state == Updater::State::UpdateAvailable) {
                        NSString* s = [NSString stringWithUTF8String:
                            std::string(st.message.begin(), st.message.end()).c_str()];
                        dispatch_async(dispatch_get_main_queue(), ^{
                            ChatViewSetTranscript(m_impl->chatView, s);
                        });
                    }
                });
        }

        Logger::Info("overlay initialized");
    }
    return true;
}

void MacOverlayWindow::RunMessageLoop() {
    [NSApp run];
}

// -----------------------------------------------------------------------------
// Send paths
// -----------------------------------------------------------------------------

static void DispatchAskAsync(MacOverlayWindowImpl* impl,
                              const std::wstring& userMessage,
                              const std::string& pngBase64,
                              const std::string& wavBase64)
{
    impl->inflightCalls.fetch_add(1);
    dispatch_async(dispatch_get_main_queue(), ^{
        ChatViewSetThinking(impl->chatView, YES);
    });
    impl->AddMessage(userMessage, true);

    // Add an empty bot bubble so streaming has something to append into.
    {
        std::lock_guard<std::mutex> lk(impl->messagesMutex);
        ChatMessage m;
        m.text = L"";
        m.isUser = false;
        std::time_t t = std::time(nullptr);
        std::tm tmv{}; localtime_r(&t, &tmv);
        m.hour = tmv.tm_hour;
        m.minute = tmv.tm_min;
        impl->messages.push_back(m);
    }

    std::vector<LLMTurn> history = impl->History();
    LLMConfig cfg = impl->config;

    std::thread([impl, userMessage, history, cfg, pngBase64, wavBase64]() {
        LLMClient::GenerateContentStreaming(
            userMessage, history, cfg, pngBase64, wavBase64,
            [impl](const std::wstring& chunk, bool isFinal) {
                if (!chunk.empty()) {
                    impl->AppendToLastBot(chunk);
                }
                if (isFinal) {
                    int remaining = impl->inflightCalls.fetch_sub(1) - 1;
                    impl->Finalize();
                    if (remaining <= 0) {
                        dispatch_async(dispatch_get_main_queue(), ^{
                            ChatViewSetThinking(impl->chatView, NO);
                        });
                    }
                }
            });
    }).detach();
}

void MacOverlayWindow::CaptureScreenOnly() {
    if (!m_impl->screenshot) return;
    std::string png = m_impl->screenshot->CaptureMonitorUnderCursorAsBase64Png();
    if (png.empty()) {
        m_impl->AddMessage(L"[screenshot failed — check Screen Recording permission]", false);
        return;
    }
    DispatchAskAsync(m_impl.get(),
                     L"[Please answer the question on this screen]",
                     png, std::string());
}

void MacOverlayWindow::CaptureAudioOnly() {
    if (!m_impl->audio) return;
    std::string wav = m_impl->audio->SnapshotAsBase64Wav(30);
    if (wav.empty()) {
        m_impl->AddMessage(L"[no audio captured yet — grant Screen Recording permission and try again]", false);
        return;
    }
    DispatchAskAsync(m_impl.get(),
                     L"[Please answer the question in the attached audio]",
                     std::string(), wav);
}

void MacOverlayWindow::UpdateFromClipboard() {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        NSString* text = [pb stringForType:NSPasteboardTypeString];
        if (!text || text.length == 0) {
            m_impl->AddMessage(L"[clipboard is empty]", false);
            return;
        }
        NSData* utf8 = [text dataUsingEncoding:NSUTF8StringEncoding];
        std::string s((const char*)[utf8 bytes], [utf8 length]);
        std::wstring w = LLMClient::Utf8ToWide(s);
        DispatchAskAsync(m_impl.get(), w, std::string(), std::string());
    }
}

void MacOverlayWindow::ResetConversation() {
    {
        std::lock_guard<std::mutex> lk(m_impl->messagesMutex);
        m_impl->messages.clear();
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        ChatViewSetMessages(m_impl->chatView, &m_impl->messages);
        [m_impl->chatView setNeedsDisplay:YES];
    });
}

void MacOverlayWindow::CopyLastAnswer() {
    std::lock_guard<std::mutex> lk(m_impl->messagesMutex);
    for (auto it = m_impl->messages.rbegin(); it != m_impl->messages.rend(); ++it) {
        if (!it->isUser && !it->text.empty()) {
            std::string utf8 = LLMClient::WideToUtf8(it->text);
            @autoreleasepool {
                NSString* s = [NSString stringWithUTF8String:utf8.c_str()];
                NSPasteboard* pb = [NSPasteboard generalPasteboard];
                [pb clearContents];
                [pb setString:s forType:NSPasteboardTypeString];
            }
            return;
        }
    }
}

void MacOverlayWindow::ToggleVisibility() {
    if ([m_impl->window isVisible]) {
        [m_impl->window orderOut:nil];
    } else {
        [m_impl->window orderFront:nil];
    }
}

void MacOverlayWindow::ToggleAutoMode() {
    bool on = !m_impl->autoMode.load();
    m_impl->autoMode.store(on);
    ChatViewSetMode(m_impl->chatView, on, m_impl->moveMode.load(), m_impl->selectMode.load());
    // Note: full auto-poll loop is a future enhancement. The toggle is
    // already wired up so the renderer can show the badge.
}

void MacOverlayWindow::ToggleMoveMode() {
    bool on = !m_impl->moveMode.load();
    m_impl->moveMode.store(on);
    [m_impl->window setIgnoresMouseEvents:!on];
    [m_impl->window setMovableByWindowBackground:on];
    ChatViewSetMode(m_impl->chatView, m_impl->autoMode.load(), on, m_impl->selectMode.load());
}

void MacOverlayWindow::ToggleSelectMode() {
    bool on = !m_impl->selectMode.load();
    m_impl->selectMode.store(on);
    [m_impl->window setIgnoresMouseEvents:!on];
    ChatViewSetMode(m_impl->chatView, m_impl->autoMode.load(), m_impl->moveMode.load(), on);
}

void MacOverlayWindow::ExitApp() {
    Logger::Info("exit requested");
    if (m_impl->audioLevelTimer) {
        [m_impl->audioLevelTimer invalidate];
        m_impl->audioLevelTimer = nil;
    }
    if (m_impl->audio) m_impl->audio->Stop();
    for (auto ref : m_impl->hotkeys) UnregisterEventHotKey(ref);
    m_impl->hotkeys.clear();
    {
        std::lock_guard<std::mutex> lk(g_hotkeyMutex);
        g_hotkeys.clear();
    }
    [NSApp terminate:nil];
}

void MacOverlayWindow::ScrollChat(int delta) {
    ChatViewScroll(m_impl->chatView, delta);
}

void MacOverlayWindow::ToggleHotkeyHints() {
    ChatViewToggleHints(m_impl->chatView);
}

void MacOverlayWindow::OpenRuntimeSettings() {
    // Unregister current Carbon hotkeys before showing the modal so the user
    // can rebind without conflicts firing.
    for (auto ref : m_impl->hotkeys) UnregisterEventHotKey(ref);
    m_impl->hotkeys.clear();
    {
        std::lock_guard<std::mutex> lk(g_hotkeyMutex);
        g_hotkeys.clear();
    }

    // Run modal — config dialog mutates m_impl->config in place.
    @autoreleasepool {
        std::vector<ModelInfo> models = ConfigLoader::LoadModels("models_list.txt");
        if (MacConfigDialog::Show(m_impl->config, models)) {
            ConfigLoader::SaveConfig("llm_config.txt", m_impl->config);
            Logger::Info("settings re-applied via runtime dialog");
        }
    }

    // Re-apply theme + hand the (possibly new) config back to the renderer.
    ChatViewSetTheme(m_impl->chatView, m_impl->config.theme.c_str());
    ChatViewSetConfig(m_impl->chatView, &m_impl->config);

    // Restart audio capture if the mic checkbox flipped.
    if (m_impl->audio) {
        m_impl->audio->Stop();
        m_impl->audio->Start(m_impl->config.capture_mic,
                             m_impl->config.audio_device_id,
                             m_impl->config.mic_device_id);
    }

    // Re-register hotkeys with the new bindings.
    int registered = 0, failed = 0;
    for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
        HotkeyBinding b = m_impl->config.hotkeys.bindings[i];
        if (b.empty()) continue;
        UInt32 mods = ConvertModifiers(b.modifiers);
        UInt32 kc   = ConvertVk(b.vk);
        if (kc == 0xFFFF) { failed++; continue; }
        EventHotKeyID hkid = { 'OVRL', (UInt32)(i + 1) };
        EventHotKeyRef ref = nullptr;
        if (RegisterEventHotKey(kc, mods, hkid, GetApplicationEventTarget(),
                                0, &ref) == noErr) {
            m_impl->hotkeys.push_back(ref);
            std::lock_guard<std::mutex> lk(g_hotkeyMutex);
            g_hotkeys.push_back({ (UInt32)(i + 1), (HotkeyAction)i, this });
            registered++;
        } else {
            failed++;
        }
    }

    // Re-register the special hardcoded hotkeys too.
    auto reSpecial = [&](UInt32 id, UInt32 mods, UInt32 kc) {
        EventHotKeyID hkid = { 'OVRL', id };
        EventHotKeyRef ref = nullptr;
        if (RegisterEventHotKey(kc, mods, hkid, GetApplicationEventTarget(),
                                0, &ref) == noErr) {
            m_impl->hotkeys.push_back(ref);
            std::lock_guard<std::mutex> lk(g_hotkeyMutex);
            g_hotkeys.push_back({ id, HotkeyAction::Count, this });
        }
    };
    reSpecial(MacOverlayWindowImpl::kHkToggleHints,  cmdKey | optionKey, kVK_ANSI_Slash);
    reSpecial(MacOverlayWindowImpl::kHkOpenSettings, cmdKey | optionKey, kVK_ANSI_Comma);
    reSpecial(MacOverlayWindowImpl::kHkShowAbout,    cmdKey | optionKey, kVK_ANSI_I);

    Logger::Info("hotkeys re-registered: " + std::to_string(registered) +
                 " ok, " + std::to_string(failed) + " failed");
}

void MacOverlayWindow::ShowAbout() {
    @autoreleasepool {
        NSAlert* a = [[NSAlert alloc] init];
        [a setMessageText:@"Invisible AI Overlay"];
        [a setInformativeText:
            @"Version 2.5.1 (macOS)\n\n"
            @"Live interview & study copilot. Captures meeting audio + "
            @"screen + clipboard text, sends to your chosen LLM, streams "
            @"the answer here. Invisible to screen recording.\n\n"
            @"⌘⌥/ to view all hotkeys · ⌘⌥, to change settings · ⌘⌥X to exit\n\n"
            @"github.com/Zer0skillman/Interview-Hack"];
        [a addButtonWithTitle:@"OK"];
        [a runModal];
    }
}
