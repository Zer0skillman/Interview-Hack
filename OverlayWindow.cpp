#include "OverlayWindow.h"
#include "ConfigDialog.h"
#include "LLMClient.h"
#include "Updater.h"
#include <algorithm> // for std::max
#include <objidl.h>
#include <gdiplus.h>
#include <wincrypt.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <shellapi.h>  // ShellExecuteW
#include <winhttp.h>   // for the update-check GET
#include <dwmapi.h>    // Win11 dark title bar + rounded corners
#include <thread>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

// Forward-declaration so methods defined before this helper can call it
static std::wstring SessionChatPath(const std::string& sessionName);

// Modal edit dialog used by "edit and resend last user message".
namespace {
struct EditDlgState {
    std::wstring currentText;
    std::wstring resultText;
    bool ok = false;
};
static EditDlgState* s_editDlgState = nullptr;
}

static LRESULT CALLBACK EditDlgProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = CreateFont(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HWND hEdit = CreateWindow(L"EDIT",
            s_editDlgState ? s_editDlgState->currentText.c_str() : L"",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL
              | ES_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP,
            10, 10, 480, 220, hwnd, (HMENU)100, NULL, NULL);
        SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hOk = CreateWindow(L"BUTTON", L"Resend",
            WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON | WS_TABSTOP,
            300, 240, 100, 28, hwnd, (HMENU)101, NULL, NULL);
        SendMessage(hOk, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hCancel = CreateWindow(L"BUTTON", L"Cancel",
            WS_VISIBLE | WS_CHILD | WS_TABSTOP,
            410, 240, 80, 28, hwnd, (HMENU)102, NULL, NULL);
        SendMessage(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);
        SetFocus(hEdit);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 101 && s_editDlgState) {
            HWND hEdit = GetDlgItem(hwnd, 100);
            int len = GetWindowTextLength(hEdit);
            std::vector<wchar_t> buf(len + 1, 0);
            GetWindowText(hEdit, buf.data(), len + 1);
            s_editDlgState->resultText = buf.data();
            s_editDlgState->ok = true;
            DestroyWindow(hwnd);
        } else if (LOWORD(w) == 102) {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:    DestroyWindow(hwnd); return 0;
    case WM_DESTROY:  PostQuitMessage(0);   return 0;
    }
    return DefWindowProc(hwnd, msg, w, l);
}

static bool ShowEditDialog(HINSTANCE hInst, HWND owner, const std::wstring& current, std::wstring& outText) {
    EditDlgState st;
    st.currentText = current;
    s_editDlgState = &st;

    WNDCLASS wc = {0};
    wc.lpfnWndProc = EditDlgProc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wc.lpszClassName = L"OverlayEditDlg";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    WNDCLASSEX wcex;
    if (!GetClassInfoEx(hInst, wc.lpszClassName, &wcex)) RegisterClass(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int w = 510, h = 310;
    HWND dhwnd = CreateWindow(wc.lpszClassName, L"Edit message and resend",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        (sw - w) / 2, (sh - h) / 2, w, h,
        owner, NULL, hInst, NULL);
    if (!dhwnd) { s_editDlgState = nullptr; return false; }

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(dhwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    bool ok = st.ok;
    if (ok) outText = st.resultText;
    s_editDlgState = nullptr;
    return ok;
}

const wchar_t* OverlayWindow::CLASS_NAME = L"InvisibleOverlayClass";
const wchar_t* OverlayWindow::WINDOW_TITLE = L"Invisible Overlay";

// ---- Named constants (formerly magic numbers) ----
// Sentinel for "scroll to the bottom"; OnPaint clamps to actual max.
static constexpr int kScrollToBottom    = 999999;
// Background polling timer ID
static constexpr UINT_PTR kPollTimerId  = 100;
// Adaptive poll intervals
static constexpr int kPollMsActive      = 3000;
static constexpr int kPollMsSilent      = 8000;
// Audio capture windows
static constexpr int kAudioSendSeconds  = 30;   // F7 / F8 manual send
static constexpr int kPollAudioSeconds  = 8;    // auto-mode poll snapshot
// VAD threshold for "is someone speaking"
static constexpr float kSpeechThreshold = 0.010f;
// Hotkey ID ranges:
//   1..HotkeyAction::Count       = user-configurable semantic hotkeys
//   105..114                     = fixed UI shortcuts (Shift+arrows, F1, F2, F11, Ctrl combos)

OverlayWindow::OverlayWindow() : m_hwnd(NULL), m_hInstance(NULL), m_scrollOffset(0), m_contentHeight(0)
{
}

OverlayWindow::~OverlayWindow()
{
    if (m_hwnd)
    {
        // Persist conversation if there's anything to save
        if (m_config.restore_session && !m_messages.empty()) {
            SaveConversation();
        }
        KillTimer(m_hwnd, 100);
        for (int id = 1; id <= (int)HotkeyAction::Count; ++id) UnregisterHotKey(m_hwnd, id);
        for (int id = 100; id <= 115; ++id) UnregisterHotKey(m_hwnd, id);
    }
}

bool OverlayWindow::Initialize(HINSTANCE hInstance)
{
    m_hInstance = hInstance;

    WNDCLASSEX wc = { };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = OverlayWindow::WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    // Dark Grey Background (RGB: 30, 30, 30)
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));

    if (!RegisterClassEx(&wc))
    {
        // Debugging: Check if standard failure or already exists
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            MessageBox(NULL, L"RegisterClassEx Failed", L"Error", MB_ICONERROR);
            return false;
        }
    }

    DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    // Use saved pos/size if present, else default to right-side panel.
    int winW = m_config.win_w > 0 ? m_config.win_w : (int)(screenW * 0.22);
    int winH = m_config.win_h > 0 ? m_config.win_h : (int)(screenH * 0.75);
    int x    = m_config.win_w > 0 ? m_config.win_x : (screenW - winW - 20);
    int y    = m_config.win_h > 0 ? m_config.win_y : 80;

    // Clamp to current screen bounds so a saved off-screen position doesn't strand the window
    if (x < -winW + 100) x = 20;
    if (y < -winH + 50)  y = 20;
    if (x > screenW - 100) x = screenW - winW - 20;
    if (y > screenH - 50)  y = screenH - winH - 20;

    m_hwnd = CreateWindowEx(
        exStyle,
        CLASS_NAME,
        WINDOW_TITLE,
        WS_POPUP,
        x, y, winW, winH,
        NULL,
        NULL,
        hInstance,
        this
    );

    if (m_hwnd == NULL)
    {
        // Helper to format error code
        DWORD err = GetLastError();
        wchar_t buf[256];
        wsprintf(buf, L"CreateWindowEx Failed. Error Code: %d", err);
        MessageBox(NULL, buf, L"Error", MB_ICONERROR);
        return false;
    }

    // INS, DEL, END, PgUp, PgDn are now configurable HotkeyActions (registered below).
    // Only the truly fixed shortcuts stay at IDs 105+ — these are tied to specific UI features
    // and don't belong in the rebind UI.
    RegisterHotKey(m_hwnd, 105, MOD_SHIFT, VK_LEFT);   // scroll code left
    RegisterHotKey(m_hwnd, 106, MOD_SHIFT, VK_RIGHT);  // scroll code right
    RegisterHotKey(m_hwnd, 107, 0, VK_F11);            // runtime settings
    // All Ctrl+letter shortcuts are global hotkeys — they'd hijack system
    // shortcuts (zoom, find, refresh, etc.) in every other app. Use Ctrl+Alt
    // combos instead — almost never bound by other apps.
    RegisterHotKey(m_hwnd, 108, MOD_CONTROL | MOD_ALT, VK_OEM_PLUS);   // Ctrl+Alt+= font bigger
    RegisterHotKey(m_hwnd, 109, MOD_CONTROL | MOD_ALT, VK_OEM_MINUS);  // Ctrl+Alt+- font smaller
    RegisterHotKey(m_hwnd, 110, MOD_CONTROL | MOD_ALT, 'E');           // Ctrl+Alt+E export
    RegisterHotKey(m_hwnd, 111, MOD_CONTROL | MOD_ALT, 'F');           // Ctrl+Alt+F search
    RegisterHotKey(m_hwnd, 112, 0, VK_F1);                              // F1 About
    RegisterHotKey(m_hwnd, 113, MOD_CONTROL | MOD_ALT, 'G');           // Ctrl+Alt+G regenerate last
    RegisterHotKey(m_hwnd, 114, 0, VK_F2);                              // F2 hotkey hints overlay
    RegisterHotKey(m_hwnd, 115, MOD_CONTROL | MOD_ALT, 'U');           // Ctrl+Alt+U install pending update

    // User-configurable semantic hotkeys (IDs 1..Count, from config)
    RegisterConfigHotkeys();

    #ifndef WDA_EXCLUDEFROMCAPTURE
    #define WDA_EXCLUDEFROMCAPTURE 0x00000011
    #endif

    if (!SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE))
    {
        SetWindowDisplayAffinity(m_hwnd, WDA_MONITOR);
    }

    // Win11 polish: dark title bar (irrelevant here since we're frameless) and
    // rounded corners (the overlay benefits visually).
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DWORD corner = 2 /*DWMWCP_ROUND*/;
        DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    }

    SetLayeredWindowAttributes(m_hwnd, 0, (BYTE)m_config.opacity_alpha, LWA_ALPHA);

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    // Start background audio capture. Loopback (system audio) always; optionally
    // mix in mic per config. Device IDs empty -> default endpoints.
    m_audio = CreateAudioCapture();
    m_audio->Start(m_config.capture_mic, m_config.audio_device_id, m_config.mic_device_id);
    m_screenshot = CreateScreenshot();

    // Load prior conversation if requested
    if (m_config.restore_session) {
        LoadConversation();
        if (!m_messages.empty()) {
            m_scrollOffset = kScrollToBottom;
            InvalidateRect(m_hwnd, NULL, TRUE);
        }
    }

    // Fire-and-forget update check (no-op if no URL set)
    CheckForUpdateAsync();

    // Polling timer for real-time transcription / auto-answer (5 seconds).
    // Fires only when provider is Gemini and a poll isn't already in flight.
    SetTimer(m_hwnd, 100, 5000, NULL);

    // Config Loaded in main
    // m_config = ConfigLoader::LoadConfig("llm_config.txt");

    return true;
}

void OverlayWindow::RunMessageLoop()
{
    MSG msg = { };
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

LRESULT CALLBACK OverlayWindow::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    OverlayWindow* pThis = NULL;

    if (uMsg == WM_CREATE)
    {
        CREATESTRUCT* pCreate = (CREATESTRUCT*)lParam;
        pThis = (OverlayWindow*)pCreate->lpCreateParams;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pThis);
    }
    else
    {
        pThis = (OverlayWindow*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    }

    switch (uMsg)
    {
    case WM_HOTKEY:
        if (pThis)
        {
            // Fixed shortcut hotkeys (105..113) — UI features not in the rebind UI
            switch (wParam) {
                case 105:
                    pThis->m_codeScrollX = std::max(0, pThis->m_codeScrollX - 60);
                    InvalidateRect(pThis->m_hwnd, NULL, TRUE);
                    return 0;
                case 106:
                    pThis->m_codeScrollX += 60;  // unbounded right; OnPaint just clips
                    InvalidateRect(pThis->m_hwnd, NULL, TRUE);
                    return 0;
                case 107: pThis->OpenRuntimeSettings(); return 0;
                case 108:  // font bigger
                    pThis->m_config.font_size_prose = std::min(36, pThis->m_config.font_size_prose + 1);
                    pThis->m_config.font_size_code  = std::min(32, pThis->m_config.font_size_code + 1);
                    ConfigLoader::SaveConfig("llm_config.txt", pThis->m_config);
                    InvalidateRect(pThis->m_hwnd, NULL, TRUE);
                    return 0;
                case 109:  // font smaller
                    pThis->m_config.font_size_prose = std::max(10, pThis->m_config.font_size_prose - 1);
                    pThis->m_config.font_size_code  = std::max(10, pThis->m_config.font_size_code - 1);
                    ConfigLoader::SaveConfig("llm_config.txt", pThis->m_config);
                    InvalidateRect(pThis->m_hwnd, NULL, TRUE);
                    return 0;
                case 110: pThis->ExportChat(); return 0;
                case 111: pThis->ToggleSearch(); return 0;
                case 112: pThis->ShowAbout(); return 0;
                case 113: pThis->RegenerateLastAnswer(); return 0;
                case 114: pThis->ToggleHotkeyHints(); return 0;
                case 115: {
                    // Install staged update + restart
                    HWND hwnd = pThis->m_hwnd;
                    Updater::InstallAndRestart([hwnd](const Updater::Status& st) {
                        std::wstring msg = st.message;
                        auto* pair = new std::pair<std::wstring, std::wstring>(msg, std::wstring());
                        PostMessage(hwnd, WM_POLL_RESULT, 0, (LPARAM)pair);
                    });
                    // Give the new process a moment to start, then exit.
                    Sleep(800);
                    PostQuitMessage(0);
                    return 0;
                }
            }
            // Configurable semantic hotkeys: ID = (HotkeyAction value) + 1
            int idx = (int)wParam - 1;
            if (idx >= 0 && idx < (int)HotkeyAction::Count) {
                switch ((HotkeyAction)idx) {
                    case HotkeyAction::SendScreen:       pThis->CaptureScreenOnly();    break;
                    case HotkeyAction::SendAudio:        pThis->CaptureAudioOnly();     break;
                    case HotkeyAction::ToggleAuto:       pThis->ToggleAutoMode();       break;
                    case HotkeyAction::MoveMode:         pThis->ToggleMoveMode();       break;
                    case HotkeyAction::ResetChat:        pThis->ResetConversation();    break;
                    case HotkeyAction::CopyAnswer:       pThis->CopyLastAnswer();       break;
                    case HotkeyAction::SelectMode:       pThis->ToggleSelectMode();     break;
                    case HotkeyAction::SendText:         pThis->UpdateFromClipboard();  break;
                    case HotkeyAction::ToggleVisibility: pThis->ToggleVisibility();     break;
                    case HotkeyAction::ExitApp:          PostQuitMessage(0);            break;
                    case HotkeyAction::ScrollUp:         pThis->Scroll(-50);            break;
                    case HotkeyAction::ScrollDown:       pThis->Scroll(50);             break;
                    default: break;
                }
            }
        }
        return 0;

    case WM_LLM_RESPONSE:
        if (pThis)
        {
             // We passed the string pointer in lParam
             std::wstring* response = (std::wstring*)lParam;
             if (response) {
                 pThis->HandleLLMResponse(*response);
                 delete response; // Clean up the heap memory allocated in the thread
             }
        }
        return 0;

    case WM_LLM_CHUNK:
        if (pThis)
        {
            std::wstring* chunk = (std::wstring*)lParam;
            if (chunk) {
                pThis->HandleLLMChunk(*chunk, wParam == 1);
                delete chunk;
            }
        }
        return 0;

    case WM_POLL_RESULT:
        if (pThis)
        {
            auto* pair = reinterpret_cast<std::pair<std::wstring, std::wstring>*>(lParam);
            if (pair) {
                pThis->HandlePollResult(pair->first, pair->second);
                delete pair;
            }
        }
        return 0;

    case WM_TIMER:
        if (pThis && wParam == 100) {
            pThis->FirePoll();
        }
        return 0;

    case WM_PAINT:
        if (pThis)
        {
            pThis->OnPaint(hwnd);
        }
        return 0;

    case WM_MOVE:
    case WM_SIZE:
        if (pThis && pThis->m_hwnd) {
            RECT r;
            if (GetWindowRect(pThis->m_hwnd, &r)) {
                pThis->m_config.win_x = r.left;
                pThis->m_config.win_y = r.top;
                pThis->m_config.win_w = r.right - r.left;
                pThis->m_config.win_h = r.bottom - r.top;
                // Save lazily — only persist when user finishes dragging (size/move via Win32 fires lots)
                // For simplicity, save every move/size. File is tiny.
                ConfigLoader::SaveConfig("llm_config.txt", pThis->m_config);
            }
        }
        break;

    case WM_LBUTTONDOWN:
        if (pThis && pThis->m_selectMode) {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            for (size_t i = 0; i < pThis->m_bubbleBounds.size() && i < pThis->m_messages.size(); ++i) {
                RECT r = pThis->m_bubbleBounds[i];
                if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
                    std::wstring text = pThis->m_messages[i].text;
                    bool isUser = pThis->m_messages[i].isUser;
                    // Strip "[tag] " prefix on user bubbles
                    if (isUser && !text.empty() && text[0] == L'[') {
                        size_t close = text.find(L"] ");
                        if (close != std::wstring::npos) text = text.substr(close + 2);
                    }

                    if (isUser) {
                        // User bubble: open edit dialog. On Resend: drop this and all
                        // subsequent messages, then dispatch as a fresh INS-style send.
                        pThis->ToggleSelectMode();  // exit select first so dialog gets focus
                        std::wstring edited;
                        if (ShowEditDialog(pThis->m_hInstance, pThis->m_hwnd, text, edited) && !edited.empty()) {
                            // Truncate history to just before this user bubble
                            pThis->m_messages.resize(i);

                            // Now push the edited user msg + thinking placeholder and fire
                            ChatMessage u; u.text = edited; u.isUser = true;
                            pThis->m_messages.push_back(u);
                            ChatMessage b; b.text = L"Thinking..."; b.isUser = false;
                            pThis->m_messages.push_back(b);
                            pThis->TrimHistory();
                            pThis->m_wasAtBottom = true;
                            pThis->m_scrollOffset = kScrollToBottom;
                            pThis->m_inflightCalls++;

                            std::vector<LLMTurn> history;
                            if (pThis->m_messages.size() >= 2) {
                                history.reserve(pThis->m_messages.size() - 2);
                                for (size_t j = 0; j + 2 < pThis->m_messages.size(); ++j) {
                                    history.push_back({ pThis->m_messages[j].isUser, pThis->m_messages[j].text });
                                }
                            }
                            LLMConfig cfg = pThis->m_config;
                            HWND hwnd = pThis->m_hwnd;
                            std::wstring q = edited;
                            std::thread([hwnd, q, history, cfg]() {
                                LLMClient::GenerateContentStreaming(q, history, cfg,
                                    std::string(), std::string(),
                                    [hwnd](const std::wstring& chunk, bool isFinal) {
                                        std::wstring* heap = new std::wstring(chunk);
                                        PostMessage(hwnd, WM_LLM_CHUNK, isFinal ? 1 : 0, (LPARAM)heap);
                                    });
                            }).detach();
                        }
                    } else {
                        // Bot bubble: copy to clipboard (existing behavior)
                        if (!text.empty() && OpenClipboard(pThis->m_hwnd)) {
                            EmptyClipboard();
                            size_t bytes = (text.size() + 1) * sizeof(wchar_t);
                            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                            if (hMem) {
                                void* dst = GlobalLock(hMem);
                                if (dst) {
                                    memcpy(dst, text.c_str(), bytes);
                                    GlobalUnlock(hMem);
                                    SetClipboardData(CF_UNICODETEXT, hMem);
                                }
                            }
                            CloseClipboard();
                        }
                        pThis->m_lastTranscript = L"Copied to clipboard";
                        pThis->ToggleSelectMode();
                    }
                    break;
                }
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (pThis) {
            if (wParam == VK_ESCAPE) {
                if (pThis->m_searchActive) { pThis->ToggleSearch(); return 0; }
                if (pThis->m_selectMode) { pThis->ToggleSelectMode(); return 0; }
                if (pThis->m_moveMode)   { pThis->ToggleMoveMode();   return 0; }
            }
            if (pThis->m_searchActive) {
                if (wParam == VK_BACK) {
                    if (!pThis->m_searchQuery.empty()) pThis->m_searchQuery.pop_back();
                    InvalidateRect(pThis->m_hwnd, NULL, TRUE);
                    return 0;
                }
                if (wParam == VK_RETURN) {
                    // Find first match and scroll to it (approximate — set scroll to roughly that bubble's Y)
                    if (!pThis->m_searchQuery.empty()) {
                        std::wstring q = pThis->m_searchQuery;
                        for (auto& c : q) c = (wchar_t)towlower(c);
                        for (size_t i = 0; i < pThis->m_messages.size(); ++i) {
                            std::wstring t = pThis->m_messages[i].text;
                            for (auto& c : t) c = (wchar_t)towlower(c);
                            if (t.find(q) != std::wstring::npos) {
                                // Best-effort: jump roughly using i / total ratio
                                if (!pThis->m_messages.empty()) {
                                    pThis->m_scrollOffset = (int)((double)i / pThis->m_messages.size() * pThis->m_contentHeight);
                                }
                                pThis->m_wasAtBottom = false;
                                InvalidateRect(pThis->m_hwnd, NULL, TRUE);
                                break;
                            }
                        }
                    }
                    return 0;
                }
            }
        }
        break;

    case WM_CHAR:
        if (pThis && pThis->m_searchActive && wParam >= 0x20 && wParam != 0x7F) {
            pThis->m_searchQuery += (wchar_t)wParam;
            InvalidateRect(pThis->m_hwnd, NULL, TRUE);
            return 0;
        }
        break;

    case WM_NCHITTEST:
        if (pThis && pThis->m_moveMode) {
            // Map screen to client to decide drag vs. resize edge
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            RECT rc; GetClientRect(hwnd, &rc);
            const int edge = 12;
            bool right  = pt.x >= rc.right - edge;
            bool bottom = pt.y >= rc.bottom - edge;
            bool left   = pt.x <= edge;
            bool top    = pt.y <= edge;
            if (right && bottom) return HTBOTTOMRIGHT;
            if (left  && bottom) return HTBOTTOMLEFT;
            if (right && top)    return HTTOPRIGHT;
            if (left  && top)    return HTTOPLEFT;
            if (right)  return HTRIGHT;
            if (bottom) return HTBOTTOM;
            if (left)   return HTLEFT;
            if (top)    return HTTOP;
            return HTCAPTION;  // anywhere else: drag the whole window
        }
        break;

    case WM_ERASEBKGND: {
        if (pThis) {
            HDC hdc = (HDC)wParam;
            RECT rc; GetClientRect(hwnd, &rc);
            auto theme = ConfigLoader::GetTheme(pThis->m_config.theme);
            HBRUSH bg = CreateSolidBrush(theme.bg);
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            return 1;
        }
        break;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void OverlayWindow::ToggleVisibility()
{
    if (IsWindowVisible(m_hwnd)) {
        ShowWindow(m_hwnd, SW_HIDE);
    } else {
        ShowWindow(m_hwnd, SW_SHOW);
    }
}

void OverlayWindow::Scroll(int delta)
{
    RECT rect;
    GetClientRect(m_hwnd, &rect);
    int windowHeight = rect.bottom - rect.top;

    m_scrollOffset += delta;

    int maxScroll = std::max(0, m_contentHeight - windowHeight + 20);

    if (m_scrollOffset < 0) m_scrollOffset = 0;
    if (m_scrollOffset > maxScroll) m_scrollOffset = maxScroll;

    // User intent: if they ended at bottom, future chunks should follow; otherwise leave them.
    m_wasAtBottom = (m_scrollOffset >= maxScroll - 5);

    InvalidateRect(m_hwnd, NULL, TRUE);
}

#include <thread>

void OverlayWindow::UpdateFromClipboard()
{
    // Ensure visible when interacting
    ShowWindow(m_hwnd, SW_SHOW);

    if (!OpenClipboard(m_hwnd))
    {
        return;
    }

    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData != NULL)
    {
        wchar_t* pszText = static_cast<wchar_t*>(GlobalLock(hData));
        if (pszText != NULL)
        {
            std::wstring clipText(pszText);
            GlobalUnlock(hData);

            // Add User Message
            ChatMessage userMsg;
            userMsg.text = clipText;
            userMsg.isUser = true;
            m_messages.push_back(userMsg);

            // Add Placeholder Bot Message
            ChatMessage botMsg;
            botMsg.text = L"Thinking..."; // Placeholder
            botMsg.isUser = false;
            m_messages.push_back(botMsg);

            TrimHistory();

            // User initiated — follow the answer down
            m_wasAtBottom = true;
            m_scrollOffset = kScrollToBottom;
            m_inflightCalls++;
            InvalidateRect(m_hwnd, NULL, TRUE);
            
            // Snapshot history (everything before the just-added user msg + placeholder)
            std::vector<LLMTurn> history;
            if (m_messages.size() >= 2) {
                history.reserve(m_messages.size() - 2);
                for (size_t i = 0; i + 2 < m_messages.size(); ++i) {
                    history.push_back({ m_messages[i].isUser, m_messages[i].text });
                }
            }

            // Start Async Request
            LLMConfig cfg = m_config; // copy
            HWND hwnd = m_hwnd;

            std::thread([hwnd, clipText, history, cfg]() {
                LLMClient::GenerateContentStreaming(clipText, history, cfg,
                    std::string(), std::string(),
                    [hwnd](const std::wstring& chunk, bool isFinal) {
                        std::wstring* heap = new std::wstring(chunk);
                        PostMessage(hwnd, WM_LLM_CHUNK, isFinal ? 1 : 0, (LPARAM)heap);
                    });
            }).detach();
        }
    }

    CloseClipboard();
}

void OverlayWindow::HandleLLMResponse(const std::wstring& response)
{
    if (!m_messages.empty() && !m_messages.back().isUser)
    {
        m_messages.back().text = response;
        RequestAutoScroll();
        InvalidateRect(m_hwnd, NULL, TRUE);
    }
}

void OverlayWindow::HandleLLMChunk(const std::wstring& chunk, bool isFinal)
{
    if (m_messages.empty() || m_messages.back().isUser) {
        if (isFinal && m_inflightCalls > 0) m_inflightCalls--;
        return;
    }

    if (!chunk.empty()) {
        if (m_messages.back().text == L"Thinking...") {
            m_messages.back().text = chunk;
        } else {
            m_messages.back().text += chunk;
        }
        AddOutputTokens((long long)chunk.size() / 4);
        RequestAutoScroll();
        InvalidateRect(m_hwnd, NULL, TRUE);
    }
    if (isFinal && m_inflightCalls > 0) {
        m_inflightCalls--;
        InvalidateRect(m_hwnd, NULL, TRUE);  // refresh the in-flight indicator
    }
}

void OverlayWindow::ToggleAutoMode()
{
    m_autoMode = !m_autoMode;
    // Reset bar text to reflect new state immediately (next FirePoll will overwrite)
    m_lastTranscript = m_autoMode ? L"(listening...)" : L"(press F9 to enable auto-answer)";
    // Reset the adaptive timer to the default cadence
    SetTimer(m_hwnd, 100, m_autoMode ? 3000 : 8000, NULL);
    InvalidateRect(m_hwnd, NULL, TRUE);
}

void OverlayWindow::UpdateClickThrough()
{
    bool transparent = !m_moveMode && !m_selectMode && !m_searchActive;
    LONG_PTR ex = GetWindowLongPtr(m_hwnd, GWL_EXSTYLE);
    if (transparent) ex |=  WS_EX_TRANSPARENT;
    else             ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtr(m_hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(m_hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    if (!transparent) SetFocus(m_hwnd);
}

void OverlayWindow::ToggleMoveMode()
{
    m_moveMode = !m_moveMode;
    if (m_moveMode) m_selectMode = false;  // mutually exclusive
    UpdateClickThrough();
    InvalidateRect(m_hwnd, NULL, TRUE);
}

void OverlayWindow::ToggleSelectMode()
{
    m_selectMode = !m_selectMode;
    if (m_selectMode) m_moveMode = false;  // mutually exclusive
    UpdateClickThrough();
    InvalidateRect(m_hwnd, NULL, TRUE);
}

void OverlayWindow::ResetConversation()
{
    m_messages.clear();
    m_scrollOffset = 0;
    m_lastAutoAnswerKey.clear();
    // Wipe the persisted file too so reset is sticky across restarts
    DeleteFileW(SessionChatPath(m_config.session_name).c_str());
    InvalidateRect(m_hwnd, NULL, TRUE);
}

void OverlayWindow::CopyLastAnswer()
{
    // Find last bot message, copy its text to clipboard
    for (auto it = m_messages.rbegin(); it != m_messages.rend(); ++it) {
        if (!it->isUser) {
            const std::wstring& text = it->text;
            if (text.empty()) return;
            if (!OpenClipboard(m_hwnd)) return;
            EmptyClipboard();
            size_t bytes = (text.size() + 1) * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (hMem) {
                void* dst = GlobalLock(hMem);
                if (dst) {
                    memcpy(dst, text.c_str(), bytes);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_UNICODETEXT, hMem);
                }
            }
            CloseClipboard();
            return;
        }
    }
}

void OverlayWindow::FirePoll()
{
    // Always update audio level for the bar dot — cheap, no API call
    m_audioLevel = m_audio->RecentEnergy(2);

    // No polling unless auto mode is on. Saves API calls when F9 is off.
    if (!m_autoMode) {
        if (m_lastTranscript != L"(press F9 to enable auto-answer)") {
            m_lastTranscript = L"(press F9 to enable auto-answer)";
            InvalidateRect(m_hwnd, NULL, TRUE);
        }
        return;
    }

    if (!LLMClient::ProviderSupportsAudio(m_config.provider)) {
        if (m_lastTranscript != L"(auto mode requires Gemini)") {
            m_lastTranscript = L"(auto mode requires Gemini)";
            InvalidateRect(m_hwnd, NULL, TRUE);
        }
        return;
    }

    // Voice activity gate: if silent, skip the poll and stretch the next tick.
    const float kSpeechThreshold = 0.010f;  // empirical; tune if needed
    bool isSpeech = m_audioLevel >= kSpeechThreshold;
    if (!isSpeech) {
        if (m_lastPollHadSpeech) {
            m_lastTranscript = L"(listening — no speech)";
            InvalidateRect(m_hwnd, NULL, TRUE);
        }
        m_lastPollHadSpeech = false;
        SetTimer(m_hwnd, 100, 8000, NULL);
        return;
    }
    if (!m_lastPollHadSpeech) {
        SetTimer(m_hwnd, 100, 3000, NULL);
    }
    m_lastPollHadSpeech = true;

    // One poll at a time
    bool expected = false;
    if (!m_pollInFlight.compare_exchange_strong(expected, true)) {
        InvalidateRect(m_hwnd, NULL, TRUE);
        return;
    }

    std::string wav = m_audio->SnapshotAsBase64Wav(8);
    if (wav.empty()) {
        m_pollInFlight.store(false);
        return;
    }

    LLMConfig cfg = m_config;
    HWND hwnd = m_hwnd;
    auto* inFlight = &m_pollInFlight;

    std::thread([hwnd, wav, cfg, inFlight]() {
        auto result = LLMClient::ClassifyAndTranscribe(wav, cfg);
        auto* pair = new std::pair<std::wstring, std::wstring>(result.transcript, result.questionText);
        PostMessage(hwnd, WM_POLL_RESULT, 0, (LPARAM)pair);
        inFlight->store(false);
    }).detach();

    InvalidateRect(m_hwnd, NULL, TRUE);
}

void OverlayWindow::HandlePollResult(const std::wstring& transcript, const std::wstring& questionText)
{
    bool changed = false;
    if (!transcript.empty() && transcript != m_lastTranscript) {
        m_lastTranscript = transcript;
        changed = true;
    }

    // Only fire an answer when auto mode is on AND a substantive question was detected
    if (m_autoMode && !questionText.empty()) {
        std::wstring key = questionText;
        for (auto& c : key) c = (wchar_t)towlower(c);
        if (key.size() > 80) key.resize(80);

        if (key != m_lastAutoAnswerKey) {
            m_lastAutoAnswerKey = key;

            ChatMessage userMsg;
            userMsg.text = L"[heard] " + questionText;
            userMsg.isUser = true;
            m_messages.push_back(userMsg);

            ChatMessage botMsg;
            botMsg.text = L"Thinking...";
            botMsg.isUser = false;
            m_messages.push_back(botMsg);

            TrimHistory();
            m_wasAtBottom = true;
            m_scrollOffset = kScrollToBottom;
            m_inflightCalls++;
            changed = true;

            if (m_config.sound_on_auto) MessageBeep(MB_OK);

            // Fire a streaming answer call so the answer streams into the bubble.
            std::vector<LLMTurn> history;
            if (m_messages.size() >= 2) {
                history.reserve(m_messages.size() - 2);
                for (size_t i = 0; i + 2 < m_messages.size(); ++i) {
                    history.push_back({ m_messages[i].isUser, m_messages[i].text });
                }
            }
            LLMConfig cfg = m_config;
            HWND hwnd = m_hwnd;
            std::wstring q = questionText;
            std::thread([hwnd, q, history, cfg]() {
                LLMClient::GenerateContentStreaming(q, history, cfg,
                    std::string(), std::string(),
                    [hwnd](const std::wstring& chunk, bool isFinal) {
                        std::wstring* heap = new std::wstring(chunk);
                        PostMessage(hwnd, WM_LLM_CHUNK, isFinal ? 1 : 0, (LPARAM)heap);
                    });
            }).detach();
        }
    }

    if (changed) InvalidateRect(m_hwnd, NULL, TRUE);
}

// Shared helper: append user+placeholder pair, snapshot history, fire streaming call.
static void DispatchAsk(
    OverlayWindow* self,
    HWND hwnd,
    std::vector<ChatMessage>& messages,
    int& scrollOffset,
    bool& wasAtBottom,
    std::atomic<int>& inflight,
    const LLMConfig& cfg,
    const std::wstring& tag,
    const std::wstring& questionText,
    const std::string& png,
    const std::string& wav)
{
    ChatMessage u; u.text = tag + questionText; u.isUser = true;
    messages.push_back(u);
    ChatMessage b; b.text = L"Thinking..."; b.isUser = false;
    messages.push_back(b);
    self->TrimHistory();
    // User initiated this — they want to follow the answer
    wasAtBottom = true;
    scrollOffset = kScrollToBottom;
    InvalidateRect(hwnd, NULL, TRUE);

    std::vector<LLMTurn> history;
    if (messages.size() >= 2) {
        history.reserve(messages.size() - 2);
        for (size_t i = 0; i + 2 < messages.size(); ++i) {
            history.push_back({ messages[i].isUser, messages[i].text });
        }
    }
    LLMConfig cfgCopy = cfg;
    std::wstring q = questionText;
    // Rough token estimate for input: history + question + system overhead
    long long estIn = 50;  // system prompt overhead
    for (const auto& t : history) estIn += t.text.size() / 4;
    estIn += q.size() / 4;
    if (!png.empty()) estIn += 1500;  // images cost roughly this many tokens
    if (!wav.empty()) estIn += 1000;
    self->AddInputTokens(estIn);

    inflight++;
    InvalidateRect(hwnd, NULL, TRUE);
    std::thread([hwnd, q, history, cfgCopy, png, wav]() {
        LLMClient::GenerateContentStreaming(q, history, cfgCopy, png, wav,
            [hwnd](const std::wstring& chunk, bool isFinal) {
                std::wstring* heap = new std::wstring(chunk);
                PostMessage(hwnd, WM_LLM_CHUNK, isFinal ? 1 : 0, (LPARAM)heap);
            });
    }).detach();
}

void OverlayWindow::CaptureScreenOnly()
{
    ShowWindow(m_hwnd, SW_SHOW);
    std::string png = m_screenshot ? m_screenshot->CaptureMonitorUnderCursorAsBase64Png() : std::string();
    std::wstring tag = png.empty() ? L"[screen capture failed] " : L"[screen] ";
    std::wstring q =
        L"Answer the question or problem shown in the attached screenshot. If it is a coding "
        L"problem, provide a working solution inside a code block, then a one-line why.";
    DispatchAsk(this, m_hwnd, m_messages, m_scrollOffset, m_wasAtBottom, m_inflightCalls, m_config, tag, q, png, std::string());
}

void OverlayWindow::CaptureAudioOnly()
{
    ShowWindow(m_hwnd, SW_SHOW);
    std::string wav = m_audio->SnapshotAsBase64Wav(30);
    if (wav.empty()) {
        ChatMessage bot; bot.isUser = false;
        bot.text = L"No audio captured yet. Play audio through your speakers and retry.";
        m_messages.push_back(bot);
        m_scrollOffset = kScrollToBottom;
        InvalidateRect(m_hwnd, NULL, TRUE);
        return;
    }

    // If current provider doesn't accept audio, fall back to Gemini for this single call
    // using the user's stored gemini_fallback_key (or main key if provider IS Gemini).
    LLMConfig cfgForCall = m_config;
    std::wstring tag = L"[audio] ";
    if (!LLMClient::ProviderSupportsAudio(m_config.provider)) {
        if (m_config.gemini_fallback_key.empty()) {
            ChatMessage bot; bot.isUser = false;
            bot.text = L"Audio needs Gemini. Set a 'Gemini fallback key' in settings, or switch provider.";
            m_messages.push_back(bot);
            m_scrollOffset = kScrollToBottom;
            InvalidateRect(m_hwnd, NULL, TRUE);
            return;
        }
        // Swap provider + key + model just for this call
        cfgForCall.provider = "gemini";
        cfgForCall.model    = "gemini-2.5-flash";
        cfgForCall.api_key  = m_config.gemini_fallback_key;
        tag = L"[audio via Gemini fallback] ";
    }

    std::wstring q =
        L"The attached audio is the last 30 seconds of the meeting. Find the interviewer's "
        L"question in it and answer it. If it's a coding problem, give working code in a code "
        L"block, then a one-line why.";
    DispatchAsk(this, m_hwnd, m_messages, m_scrollOffset, m_wasAtBottom, m_inflightCalls, cfgForCall, tag, q, std::string(), wav);
}

void OverlayWindow::RegisterConfigHotkeys()
{
    for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
        UnregisterHotKey(m_hwnd, i + 1);
        const HotkeyBinding& b = m_config.hotkeys.bindings[i];
        if (!b.empty()) {
            RegisterHotKey(m_hwnd, i + 1, b.modifiers, b.vk);
        }
    }
}

void OverlayWindow::TrimHistory()
{
    int cap = m_config.history_cap > 0 ? m_config.history_cap : 200;
    while ((int)m_messages.size() > cap) {
        m_messages.erase(m_messages.begin());
    }
}

void OverlayWindow::RequestAutoScroll()
{
    if (m_wasAtBottom) {
        m_scrollOffset = kScrollToBottom;
    }
}

// Background HTTPS GET that compares "tag_name" from the response JSON to our
// embedded version. If newer, posts a transcript-bar notice. Best-effort —
// silently no-op on any error.
static void DoUpdateCheck(HWND hwnd, std::string url) {
    if (url.empty()) return;
    if (url.rfind("https://", 0) != 0) return;  // require HTTPS

    std::string body = url.substr(8);  // strip https://
    size_t slash = body.find('/');
    std::wstring host, path;
    {
        std::string hostPart = (slash == std::string::npos) ? body : body.substr(0, slash);
        std::string pathPart = (slash == std::string::npos) ? "/" : body.substr(slash);
        int n;
        n = MultiByteToWideChar(CP_UTF8, 0, hostPart.data(), (int)hostPart.size(), NULL, 0);
        host.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, hostPart.data(), (int)hostPart.size(), &host[0], n);
        n = MultiByteToWideChar(CP_UTF8, 0, pathPart.data(), (int)pathPart.size(), NULL, 0);
        path.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, pathPart.data(), (int)pathPart.size(), &path[0], n);
    }

    HINTERNET hSession = WinHttpOpen(L"AIOverlay/2.5.1 UpdateCheck",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return;
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 10000);
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return; }

    BOOL ok = WinHttpSendRequest(hRequest,
        L"Accept: application/json\r\nUser-Agent: AIOverlay/2.3.0",
        -1L, NULL, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, NULL);
    std::string resp;
    if (ok) {
        DWORD avail = 0, downloaded = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            std::vector<char> buf(avail + 1, 0);
            if (!WinHttpReadData(hRequest, buf.data(), avail, &downloaded)) break;
            resp.append(buf.data(), downloaded);
            if (resp.size() > 200000) break;  // hard cap
        }
    }
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    // Find "tag_name":"vX.Y.Z" — keep this dumb; no JSON parser dep
    size_t k = resp.find("\"tag_name\"");
    if (k == std::string::npos) return;
    k = resp.find('"', k + 11);
    if (k == std::string::npos) return;
    size_t end = resp.find('"', k + 1);
    if (end == std::string::npos) return;
    std::string tag = resp.substr(k + 1, end - k - 1);

    // Compare to our own "2.3.0" semver. Strip leading v.
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag = tag.substr(1);
    auto parseVer = [](const std::string& s, int& a, int& b, int& c) {
        a = b = c = 0;
        sscanf(s.c_str(), "%d.%d.%d", &a, &b, &c);
    };
    int ra, rb, rc, ma = 2, mb = 4, mc = 3;
    parseVer(tag, ra, rb, rc);
    bool newer = (ra > ma) || (ra == ma && rb > mb) || (ra == ma && rb == mb && rc > mc);
    if (!newer) return;

    std::wstring notice = L"Update available: v";
    int n = MultiByteToWideChar(CP_UTF8, 0, tag.data(), (int)tag.size(), NULL, 0);
    std::wstring wtag(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, tag.data(), (int)tag.size(), &wtag[0], n);
    notice += wtag;

    // Drop it into the transcript bar as a one-shot
    std::wstring* heap = new std::wstring(notice);
    PostMessage(hwnd, WM_POLL_RESULT, 0, (LPARAM)new std::pair<std::wstring, std::wstring>(*heap, std::wstring()));
    delete heap;
}

void OverlayWindow::CheckForUpdateAsync()
{
    if (m_config.update_check_url.empty()) return;
    HWND hwnd = m_hwnd;

    // New flow: full updater (check → download → ready). Status updates go to
    // the transcript bar via WM_POLL_RESULT with empty questionText.
    Updater::CheckAndDownloadAsync(m_config.update_check_url, L"2.5.1",
        [hwnd](const Updater::Status& st) {
            // Bridge to UI thread by reusing WM_POLL_RESULT (transcript-only update).
            std::wstring msg = st.message;
            auto* pair = new std::pair<std::wstring, std::wstring>(msg, std::wstring());
            PostMessage(hwnd, WM_POLL_RESULT, 0, (LPARAM)pair);
        });
}

void OverlayWindow::ToggleHotkeyHints()
{
    m_hintsVisible = !m_hintsVisible;
    InvalidateRect(m_hwnd, NULL, TRUE);
}

void OverlayWindow::RegenerateLastAnswer()
{
    // Drop the last bot reply (if any) and re-fire with the last user question.
    if (m_messages.size() < 2) return;
    if (m_messages.back().isUser) return;  // no bot reply to regenerate
    // Find the last user message preceding the bot reply
    int botIdx = (int)m_messages.size() - 1;
    int userIdx = botIdx - 1;
    if (userIdx < 0 || !m_messages[userIdx].isUser) return;

    std::wstring question = m_messages[userIdx].text;
    // Strip leading "[tag] " prefix
    if (!question.empty() && question[0] == L'[') {
        size_t close = question.find(L"] ");
        if (close != std::wstring::npos) question = question.substr(close + 2);
    }

    // Replace bot reply with "Thinking..." and re-fire
    m_messages.back().text = L"Thinking...";
    m_messages.back().hour = -1;
    m_messages.back().minute = -1;
    m_wasAtBottom = true;
    m_scrollOffset = kScrollToBottom;
    InvalidateRect(m_hwnd, NULL, TRUE);

    std::vector<LLMTurn> history;
    history.reserve(userIdx);
    for (int i = 0; i < userIdx; ++i) {
        history.push_back({ m_messages[i].isUser, m_messages[i].text });
    }
    LLMConfig cfg = m_config;
    HWND hwnd = m_hwnd;
    m_inflightCalls++;
    std::thread([hwnd, question, history, cfg]() {
        LLMClient::GenerateContentStreaming(question, history, cfg,
            std::string(), std::string(),
            [hwnd](const std::wstring& chunk, bool isFinal) {
                std::wstring* heap = new std::wstring(chunk);
                PostMessage(hwnd, WM_LLM_CHUNK, isFinal ? 1 : 0, (LPARAM)heap);
            });
    }).detach();
}

void OverlayWindow::ShowAbout()
{
    MessageBoxW(m_hwnd,
        L"Invisible AI Overlay\n"
        L"Version 2.5.1\n\n"
        L"Live interview & study copilot.\n"
        L"Captures system audio, screenshots, clipboard text.\n"
        L"Streams answers from Gemini / Claude / OpenAI / etc.\n\n"
        L"Hidden from screen capture (WDA_EXCLUDEFROMCAPTURE).\n\n"
        L"License: MIT\n"
        L"Press F11 for runtime settings.",
        L"About Invisible AI Overlay",
        MB_ICONINFORMATION | MB_OK);
}

void OverlayWindow::ExportChat()
{
    if (m_messages.empty()) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t fname[128];
    wsprintfW(fname, L"chat_export_%04d%02d%02d_%02d%02d.md",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);

    HANDLE hFile = CreateFileW(fname, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    // UTF-8 BOM for editors
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    DWORD written;
    WriteFile(hFile, bom, 3, &written, NULL);

    auto writeStr = [&](const std::string& s) {
        WriteFile(hFile, s.data(), (DWORD)s.size(), &written, NULL);
    };
    auto wToUtf8 = [](const std::wstring& w) {
        if (w.empty()) return std::string();
        int sz = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
        std::string s(sz, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], sz, NULL, NULL);
        return s;
    };

    writeStr("# Chat export\n\n");
    for (const auto& msg : m_messages) {
        writeStr(msg.isUser ? "**You**\n\n" : "**AI**\n\n");
        writeStr(wToUtf8(msg.text));
        writeStr("\n\n---\n\n");
    }
    CloseHandle(hFile);

    // Open in default markdown viewer
    ShellExecuteW(NULL, L"open", fname, NULL, NULL, SW_SHOWNORMAL);
}

void OverlayWindow::ToggleSearch()
{
    m_searchActive = !m_searchActive;
    if (!m_searchActive) m_searchQuery.clear();
    UpdateClickThrough();
    InvalidateRect(m_hwnd, NULL, TRUE);
}

// Compose the chat filename for the active session
static std::wstring SessionChatPath(const std::string& sessionName)
{
    std::string name = sessionName.empty() ? "default" : sessionName;
    // Sanitize: only [A-Za-z0-9._-]
    std::string clean;
    for (char c : name) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '.' || c == '_' || c == '-') clean += c;
    }
    if (clean.empty()) clean = "default";
    std::string path = "chat." + clean + ".txt";
    int sz = MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), NULL, 0);
    std::wstring w(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), &w[0], sz);
    return w;
}

void OverlayWindow::SaveConversation()
{
    std::wstring path = SessionChatPath(m_config.session_name);
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    auto wToUtf8 = [](const std::wstring& w) {
        if (w.empty()) return std::string();
        int sz = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
        std::string s(sz, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], sz, NULL, NULL);
        return s;
    };
    DWORD written;
    for (const auto& msg : m_messages) {
        // Format: ROLE\tBODY_WITH_\n_ESCAPED\n
        std::string role = msg.isUser ? "U\t" : "B\t";
        std::string body = wToUtf8(msg.text);
        // Escape \n as literal \\n so each message is one line
        std::string esc;
        esc.reserve(body.size());
        for (char c : body) {
            if (c == '\n')      esc += "\\n";
            else if (c == '\r') {}
            else if (c == '\\') esc += "\\\\";
            else                esc += c;
        }
        std::string line = role + esc + "\n";
        WriteFile(hFile, line.data(), (DWORD)line.size(), &written, NULL);
    }
    CloseHandle(hFile);
}

void OverlayWindow::LoadConversation()
{
    std::wstring path = SessionChatPath(m_config.session_name);
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz; GetFileSizeEx(hFile, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > 5 * 1024 * 1024) { CloseHandle(hFile); return; }
    std::string buf((size_t)sz.QuadPart, 0);
    DWORD read;
    ReadFile(hFile, &buf[0], (DWORD)sz.QuadPart, &read, NULL);
    CloseHandle(hFile);

    auto utf8ToW = [](const std::string& s) {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
        return w;
    };

    size_t i = 0;
    while (i < buf.size()) {
        size_t nl = buf.find('\n', i);
        if (nl == std::string::npos) nl = buf.size();
        std::string line = buf.substr(i, nl - i);
        i = nl + 1;
        if (line.size() < 2) continue;
        bool isUser = (line[0] == 'U');
        std::string body = line.substr(2);
        // Unescape
        std::string un;
        un.reserve(body.size());
        for (size_t j = 0; j < body.size(); ++j) {
            if (body[j] == '\\' && j + 1 < body.size()) {
                if (body[j+1] == 'n') { un += '\n'; j++; }
                else if (body[j+1] == '\\') { un += '\\'; j++; }
                else un += body[j];
            } else un += body[j];
        }
        ChatMessage m;
        m.isUser = isUser;
        m.text = utf8ToW(un);
        m.hour = -1; m.minute = -1;  // historical, no timestamp
        m_messages.push_back(m);
    }
}

void OverlayWindow::OpenRuntimeSettings()
{
    // Re-open the welcome dialog mid-session, then apply any changes to the live overlay.
    bool oldMic = m_config.capture_mic;
    std::string oldSession = m_config.session_name;
    std::vector<ModelInfo> models = ConfigLoader::LoadModels("models_list.txt");

    if (!ConfigDialog::Show(m_hInstance, m_config, models)) {
        return;  // user cancelled — keep current settings
    }

    // Session switch: save current under old name, load new
    if (m_config.session_name != oldSession) {
        std::string newName = m_config.session_name;
        m_config.session_name = oldSession;
        if (m_config.restore_session && !m_messages.empty()) SaveConversation();
        m_config.session_name = newName;
        m_messages.clear();
        m_scrollOffset = 0;
        if (m_config.restore_session) LoadConversation();
    }

    // Persist (the dialog also saves, but be defensive)
    ConfigLoader::SaveConfig("llm_config.txt", m_config);

    // Re-register hotkeys with any new bindings
    RegisterConfigHotkeys();

    // If mic toggle OR any device ID changed, restart the audio thread
    // (we don't track old device IDs here; just restart if any audio setting could have changed)
    m_audio->Stop();
    m_audio->Start(m_config.capture_mic, m_config.audio_device_id, m_config.mic_device_id);
    (void)oldMic;

    // Reset transcript bar so old text doesn't linger from a stale provider
    m_lastTranscript.clear();
    InvalidateRect(m_hwnd, NULL, TRUE);
}

// (old DEAD_REMOVED_BODY block deleted)

// Screenshot capture moved to Screenshot_Win.cpp (behind IScreenshot interface).

// Rendering moved to Overlay_Rendering.cpp.
