#include "OverlayWindow.h"
#include "ConfigDialog.h"
#include "LLMClient.h"
#include <algorithm> // for std::max
#include <objidl.h>
#include <gdiplus.h>
#include <wincrypt.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <shellapi.h>  // ShellExecuteW
#include <winhttp.h>   // for the update-check GET

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
        for (int id = 100; id <= 114; ++id) UnregisterHotKey(m_hwnd, id);
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
    RegisterHotKey(m_hwnd, 108, MOD_CONTROL, VK_OEM_PLUS);   // Ctrl+= font bigger
    RegisterHotKey(m_hwnd, 109, MOD_CONTROL, VK_OEM_MINUS);  // Ctrl+- font smaller
    RegisterHotKey(m_hwnd, 110, MOD_CONTROL, 'E');           // Ctrl+E export
    RegisterHotKey(m_hwnd, 111, MOD_CONTROL, 'F');           // Ctrl+F search
    RegisterHotKey(m_hwnd, 112, 0, VK_F1);                   // F1 About
    RegisterHotKey(m_hwnd, 113, MOD_CONTROL | MOD_SHIFT, 'R'); // Ctrl+Shift+R regenerate last
    RegisterHotKey(m_hwnd, 114, 0, VK_F2);                     // F2 hotkey hints overlay

    // User-configurable semantic hotkeys (IDs 1..Count, from config)
    RegisterConfigHotkeys();

    #ifndef WDA_EXCLUDEFROMCAPTURE
    #define WDA_EXCLUDEFROMCAPTURE 0x00000011
    #endif

    if (!SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE))
    {
        SetWindowDisplayAffinity(m_hwnd, WDA_MONITOR);
    }

    SetLayeredWindowAttributes(m_hwnd, 0, (BYTE)m_config.opacity_alpha, LWA_ALPHA);

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    // Start background audio capture. Loopback (system audio) always; optionally
    // mix in mic per config. Device IDs empty -> default endpoints.
    m_audio.Start(m_config.capture_mic, m_config.audio_device_id, m_config.mic_device_id);

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
    m_audioLevel = m_audio.RecentEnergy(2);

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

    std::string wav = m_audio.SnapshotAsBase64Wav(8);
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
    std::string png = CaptureScreenshotAsBase64Png();
    std::wstring tag = png.empty() ? L"[screen capture failed] " : L"[screen] ";
    std::wstring q =
        L"Answer the question or problem shown in the attached screenshot. If it is a coding "
        L"problem, provide a working solution inside a code block, then a one-line why.";
    DispatchAsk(this, m_hwnd, m_messages, m_scrollOffset, m_wasAtBottom, m_inflightCalls, m_config, tag, q, png, std::string());
}

void OverlayWindow::CaptureAudioOnly()
{
    ShowWindow(m_hwnd, SW_SHOW);
    std::string wav = m_audio.SnapshotAsBase64Wav(30);
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

    HINTERNET hSession = WinHttpOpen(L"AIOverlay/2.3.0 UpdateCheck",
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
    int ra, rb, rc, ma = 2, mb = 3, mc = 0;
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
    std::string url = m_config.update_check_url;
    std::thread([hwnd, url]() { DoUpdateCheck(hwnd, url); }).detach();
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
        L"Version 2.1.0\n\n"
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
    m_audio.Stop();
    m_audio.Start(m_config.capture_mic, m_config.audio_device_id, m_config.mic_device_id);
    (void)oldMic;

    // Reset transcript bar so old text doesn't linger from a stale provider
    m_lastTranscript.clear();
    InvalidateRect(m_hwnd, NULL, TRUE);
}

// (old DEAD_REMOVED_BODY block deleted)

// PNG encoder CLSID for GDI+ Image::Save
static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    std::vector<BYTE> buf(size);
    Gdiplus::ImageCodecInfo* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, codecs);

    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, format) == 0) {
            *pClsid = codecs[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

std::string OverlayWindow::CaptureScreenshotAsBase64Png()
{
    // Pick the monitor containing the cursor
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfo(hMon, &mi)) return std::string();

    int x = mi.rcMonitor.left;
    int y = mi.rcMonitor.top;
    int w = mi.rcMonitor.right - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);
    HBITMAP hBmp  = CreateCompatibleBitmap(hdcScreen, w, h);
    HBITMAP hOld  = (HBITMAP)SelectObject(hdcMem, hBmp);

    BOOL ok = BitBlt(hdcMem, 0, 0, w, h, hdcScreen, x, y, SRCCOPY);

    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    if (!ok) {
        DeleteObject(hBmp);
        return std::string();
    }

    // Encode PNG into an in-memory IStream via GDI+
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK) {
        DeleteObject(hBmp);
        return std::string();
    }

    std::string base64;
    {
        Gdiplus::Bitmap bmp(hBmp, NULL);
        CLSID pngClsid;
        if (GetEncoderClsid(L"image/png", &pngClsid) >= 0 &&
            bmp.Save(stream, &pngClsid, NULL) == Gdiplus::Ok)
        {
            // Get raw bytes from the stream's HGLOBAL
            HGLOBAL hg = NULL;
            if (GetHGlobalFromStream(stream, &hg) == S_OK && hg) {
                SIZE_T sz = GlobalSize(hg);
                void* p = GlobalLock(hg);
                if (p && sz > 0) {
                    DWORD b64Len = 0;
                    // First call: get required length
                    if (CryptBinaryToStringA((const BYTE*)p, (DWORD)sz,
                            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                            NULL, &b64Len) && b64Len > 0)
                    {
                        base64.resize(b64Len);
                        if (CryptBinaryToStringA((const BYTE*)p, (DWORD)sz,
                                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                                &base64[0], &b64Len))
                        {
                            // CryptBinaryToStringA returns length including null terminator
                            if (!base64.empty() && base64.back() == '\0') base64.pop_back();
                        } else {
                            base64.clear();
                        }
                    }
                    GlobalUnlock(hg);
                }
            }
        }
    }

    stream->Release();
    DeleteObject(hBmp);
    return base64;
}

// Rendering moved to Overlay_Rendering.cpp.
#if 0  // start of moved-out rendering block
static const COLORREF kDefaultCodeColor   = RGB(220, 220, 220);
static const COLORREF kUnmatchedColor     = RGB(255,  80,  80);
static const COLORREF kBracketPalette[] = {
    RGB(255, 215,  64),  // yellow
    RGB(255, 120, 200),  // pink
    RGB(120, 200, 255),  // cyan
    RGB(160, 255, 160),  // light green
};

static const COLORREF kKeywordColor = RGB(200, 130, 255);  // purple
static const COLORREF kStringColor  = RGB(255, 170,  80);  // orange
static const COLORREF kNumberColor  = RGB(120, 200, 255);  // cyan
static const COLORREF kCommentColor = RGB(120, 140, 120);  // dim gray-green

// Match a word against a fixed array of keywords
static bool InSet(const std::wstring& w, const wchar_t* const* set, size_t count) {
    for (size_t i = 0; i < count; ++i) if (w == set[i]) return true;
    return false;
}

// Pan-language fallback (used when the code fence has no language label).
static bool IsKeyword(const std::wstring& w) {
    static const wchar_t* kw[] = {
        L"if", L"else", L"elif", L"for", L"while", L"do", L"switch", L"case",
        L"break", L"continue", L"return", L"default", L"goto", L"in", L"of", L"is", L"not", L"and", L"or",
        L"def", L"class", L"function", L"fn", L"func", L"var", L"let", L"const",
        L"int", L"void", L"char", L"bool", L"float", L"double", L"long", L"short", L"unsigned", L"signed",
        L"public", L"private", L"protected", L"static", L"virtual", L"override", L"final", L"abstract",
        L"new", L"delete", L"this", L"self", L"super", L"import", L"from", L"package", L"namespace", L"using",
        L"true", L"false", L"null", L"nil", L"None", L"True", L"False", L"undefined",
        L"try", L"catch", L"finally", L"throw", L"throws", L"raise", L"except",
        L"async", L"await", L"yield", L"struct", L"enum", L"interface", L"type", L"impl",
        L"auto", L"std", L"size_t",
    };
    return InSet(w, kw, sizeof(kw) / sizeof(kw[0]));
}

// Per-language keyword check. Falls back to IsKeyword() when language is empty
// or unknown.
static bool IsKeywordInLang(const std::wstring& w, const std::wstring& lang) {
    if (lang.empty()) return IsKeyword(w);

    std::wstring lk;
    for (wchar_t c : lang) lk += (wchar_t)towlower(c);

    static const wchar_t* py[] = {
        L"if", L"elif", L"else", L"for", L"while", L"break", L"continue",
        L"def", L"class", L"return", L"yield", L"import", L"from", L"as",
        L"try", L"except", L"finally", L"raise", L"with", L"lambda",
        L"True", L"False", L"None", L"and", L"or", L"not", L"in", L"is",
        L"pass", L"global", L"nonlocal", L"async", L"await",
    };
    static const wchar_t* js[] = {
        L"var", L"let", L"const", L"function", L"return", L"if", L"else", L"for", L"while",
        L"do", L"switch", L"case", L"default", L"break", L"continue", L"class", L"extends",
        L"new", L"this", L"typeof", L"instanceof", L"in", L"of", L"async", L"await",
        L"try", L"catch", L"finally", L"throw", L"true", L"false", L"null", L"undefined",
        L"import", L"export", L"from", L"as", L"interface", L"type", L"enum", L"namespace",
    };
    static const wchar_t* c_family[] = {
        L"if", L"else", L"for", L"while", L"do", L"switch", L"case", L"default", L"break",
        L"continue", L"return", L"goto", L"class", L"struct", L"enum", L"union", L"public",
        L"private", L"protected", L"static", L"virtual", L"override", L"final", L"abstract",
        L"new", L"delete", L"this", L"super", L"void", L"int", L"char", L"float", L"double",
        L"bool", L"long", L"short", L"unsigned", L"signed", L"const", L"volatile", L"auto",
        L"true", L"false", L"null", L"nullptr", L"namespace", L"using", L"typedef",
        L"try", L"catch", L"throw", L"throws", L"template", L"typename", L"sizeof",
    };
    static const wchar_t* rust[] = {
        L"fn", L"let", L"mut", L"if", L"else", L"for", L"while", L"loop", L"match",
        L"return", L"break", L"continue", L"struct", L"enum", L"impl", L"trait",
        L"pub", L"use", L"mod", L"crate", L"self", L"Self", L"as", L"in", L"where",
        L"const", L"static", L"true", L"false", L"None", L"Some", L"Ok", L"Err",
        L"async", L"await", L"move", L"ref", L"dyn", L"box",
    };
    static const wchar_t* go[] = {
        L"func", L"var", L"const", L"type", L"struct", L"interface", L"if", L"else",
        L"for", L"range", L"switch", L"case", L"default", L"break", L"continue",
        L"return", L"go", L"select", L"chan", L"defer", L"map", L"package", L"import",
        L"nil", L"true", L"false", L"iota",
    };

    if (lk == L"py"   || lk == L"python")                                 return InSet(w, py,       sizeof(py)/sizeof(*py));
    if (lk == L"js"   || lk == L"javascript" || lk == L"jsx"
        || lk == L"ts" || lk == L"typescript" || lk == L"tsx")            return InSet(w, js,       sizeof(js)/sizeof(*js));
    if (lk == L"c"    || lk == L"cpp" || lk == L"c++" || lk == L"cxx"
        || lk == L"java" || lk == L"cs"  || lk == L"csharp" || lk == L"c#") return InSet(w, c_family, sizeof(c_family)/sizeof(*c_family));
    if (lk == L"rust" || lk == L"rs")                                     return InSet(w, rust,     sizeof(rust)/sizeof(*rust));
    if (lk == L"go"   || lk == L"golang")                                 return InSet(w, go,       sizeof(go)/sizeof(*go));

    return IsKeyword(w);  // unknown language → pan-language fallback
}

static std::vector<COLORREF> ColorizeBrackets(const std::wstring& code, const std::wstring& language = std::wstring()) {
    std::vector<COLORREF> out(code.size(), kDefaultCodeColor);
    std::vector<int> stack;
    int depth = 0;
    constexpr int N = (int)(sizeof(kBracketPalette) / sizeof(kBracketPalette[0]));

    enum State { Default, InStrDQ, InStrSQ, InLineComment, InBlockComment };
    State st = Default;

    size_t i = 0;
    while (i < code.size()) {
        wchar_t c = code[i];

        if (st == InLineComment) {
            out[i] = kCommentColor;
            if (c == L'\n') st = Default;
            i++; continue;
        }
        if (st == InBlockComment) {
            out[i] = kCommentColor;
            if (c == L'*' && i + 1 < code.size() && code[i+1] == L'/') {
                out[i+1] = kCommentColor;
                i += 2;
                st = Default;
                continue;
            }
            i++; continue;
        }
        if (st == InStrDQ || st == InStrSQ) {
            out[i] = kStringColor;
            if (c == L'\\' && i + 1 < code.size()) { out[i+1] = kStringColor; i += 2; continue; }
            if ((st == InStrDQ && c == L'"') || (st == InStrSQ && c == L'\'')) st = Default;
            i++; continue;
        }

        // State == Default
        if (c == L'"') { st = InStrDQ; out[i] = kStringColor; i++; continue; }
        if (c == L'\'') { st = InStrSQ; out[i] = kStringColor; i++; continue; }
        if (c == L'/' && i + 1 < code.size() && code[i+1] == L'/') {
            st = InLineComment; out[i] = kCommentColor; i++; continue;
        }
        if (c == L'/' && i + 1 < code.size() && code[i+1] == L'*') {
            st = InBlockComment; out[i] = kCommentColor; out[i+1] = kCommentColor; i += 2; continue;
        }
        if (c == L'#') { st = InLineComment; out[i] = kCommentColor; i++; continue; }

        if (c == L'(' || c == L'[' || c == L'{') {
            out[i] = kBracketPalette[depth % N];
            stack.push_back(depth);
            depth++;
            i++; continue;
        }
        if (c == L')' || c == L']' || c == L'}') {
            if (!stack.empty()) {
                depth = stack.back();
                stack.pop_back();
                out[i] = kBracketPalette[depth % N];
            } else {
                out[i] = kUnmatchedColor;
            }
            i++; continue;
        }

        // Number literal
        if (iswdigit(c)) {
            size_t start = i;
            while (i < code.size() && (iswdigit(code[i]) || code[i] == L'.' || code[i] == L'x'
                   || (code[i] >= L'a' && code[i] <= L'f')
                   || (code[i] >= L'A' && code[i] <= L'F'))) {
                out[i] = kNumberColor;
                i++;
            }
            (void)start;
            continue;
        }

        // Identifier — color as keyword if it matches
        if (iswalpha(c) || c == L'_') {
            size_t start = i;
            while (i < code.size() && (iswalnum(code[i]) || code[i] == L'_')) i++;
            std::wstring word = code.substr(start, i - start);
            if (IsKeywordInLang(word, language)) {
                for (size_t k = start; k < i; ++k) out[k] = kKeywordColor;
            }
            continue;
        }

        i++;
    }
    return out;
}
// Quick check: does the prose contain any markdown markers worth rendering styled?
static bool HasInlineMd(const std::wstring& s) {
    return s.find(L"**") != std::wstring::npos
        || s.find(L"*")  != std::wstring::npos
        || s.find(L"_")  != std::wstring::npos
        || s.find(L"`")  != std::wstring::npos;
}

// Parse prose into runs of {text, bold, italic}. Asterisks/underscores adjacent
// to alphanumerics are treated as literal (so a*b and snake_case survive).
struct MdRun { std::wstring text; bool bold; bool italic; };
static std::vector<MdRun> ParseMdRuns(const std::wstring& s) {
    std::vector<MdRun> out;
    bool bold = false, italic = false;
    std::wstring buf;
    auto flush = [&]() {
        if (!buf.empty()) {
            out.push_back({ buf, bold, italic });
            buf.clear();
        }
    };
    size_t i = 0;
    while (i < s.size()) {
        wchar_t c = s[i];
        if (c == L'*' && i + 1 < s.size() && s[i+1] == L'*') {
            flush(); bold = !bold; i += 2; continue;
        }
        if (c == L'*' || c == L'_') {
            bool prevAlnum = (i > 0) && (iswalnum(s[i-1]) || s[i-1] == L'_');
            bool nextAlnum = (i + 1 < s.size()) && (iswalnum(s[i+1]) || s[i+1] == L'_');
            if (!(prevAlnum && nextAlnum)) {
                flush(); italic = !italic; i++; continue;
            }
        }
        if (c == L'`') { i++; continue; }  // inline code marker — stripped
        buf += c;
        i++;
    }
    flush();
    return out;
}

// Word-wrap layout with mixed fonts. Returns (width, height) used. If `draw`,
// emits TextOut calls — otherwise just measures via GetTextExtentPoint32.
//
// fonts indexed by (bold<<0 | italic<<1): 0=plain, 1=bold, 2=italic, 3=bold+italic.
struct MdLayoutResult { int width; int height; };
static MdLayoutResult LayoutMd(
    HDC hdc, const std::vector<MdRun>& runs,
    int xStart, int yStart, int maxWidth, int lineH,
    HFONT fonts[4], COLORREF textColor, bool draw)
{
    int x = xStart;
    int y = yStart;
    int maxX = xStart;
    bool anyDrawn = false;

    for (const auto& run : runs) {
        int idx = (run.bold ? 1 : 0) | (run.italic ? 2 : 0);
        SelectObject(hdc, fonts[idx]);
        if (draw) SetTextColor(hdc, textColor);

        size_t i = 0;
        while (i < run.text.size()) {
            // Skip and emit any non-newline whitespace (preserve spaces inline)
            size_t ws = i;
            while (i < run.text.size() && iswspace(run.text[i])) {
                if (run.text[i] == L'\n') {
                    if (i > ws) {
                        std::wstring sp = run.text.substr(ws, i - ws);
                        SIZE spSz; GetTextExtentPoint32W(hdc, sp.c_str(), (int)sp.size(), &spSz);
                        if (draw && x > xStart) TextOutW(hdc, x, y, sp.c_str(), (int)sp.size());
                        x += spSz.cx;
                        if (x > maxX) maxX = x;
                    }
                    x = xStart;
                    y += lineH;
                    ws = i + 1;
                }
                i++;
            }
            if (i > ws) {
                std::wstring sp = run.text.substr(ws, i - ws);
                if (!sp.empty() && x > xStart) {
                    SIZE spSz; GetTextExtentPoint32W(hdc, sp.c_str(), (int)sp.size(), &spSz);
                    if (draw) TextOutW(hdc, x, y, sp.c_str(), (int)sp.size());
                    x += spSz.cx;
                }
            }

            // Collect next word (non-whitespace)
            size_t wstart = i;
            while (i < run.text.size() && !iswspace(run.text[i])) i++;
            if (i > wstart) {
                std::wstring word = run.text.substr(wstart, i - wstart);
                SIZE sz; GetTextExtentPoint32W(hdc, word.c_str(), (int)word.size(), &sz);
                if (x + sz.cx > xStart + maxWidth && x > xStart) {
                    x = xStart;
                    y += lineH;
                }
                if (draw) {
                    TextOutW(hdc, x, y, word.c_str(), (int)word.size());
                    anyDrawn = true;
                }
                x += sz.cx;
                if (x > maxX) maxX = x;
            }
        }
    }
    (void)anyDrawn;
    return { maxX - xStart, (y - yStart) + lineH };
}

// Strip markdown inline markers (**, *, _, `) from prose for display.
// Keep the source text in m_messages intact so history sent to the API preserves
// the original markdown.
static std::wstring StripInlineMd(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        wchar_t c = s[i];
        // Bold (**) - strip both asterisks
        if (c == L'*' && i + 1 < s.size() && s[i+1] == L'*') { i += 2; continue; }
        // Italic (single * or _) - strip
        if (c == L'*' || c == L'_') {
            // Only strip if surrounded by word characters (avoid stripping things like "a_b")
            bool prevAlnum = (i > 0) && (iswalnum(s[i-1]) || s[i-1] == L'_');
            bool nextAlnum = (i + 1 < s.size()) && (iswalnum(s[i+1]) || s[i+1] == L'_');
            if (!(prevAlnum && nextAlnum)) { i++; continue; }
        }
        // Inline backtick `code` - strip
        if (c == L'`') { i++; continue; }
        out += c;
        i++;
    }
    return out;
}

static std::vector<PaintSeg> ParseSegments(const std::wstring& raw) {
    std::vector<PaintSeg> out;
    std::wstring buf;
    std::wstring pendingLang;  // language for the NEXT code segment we flush
    bool inCode = false;

    auto trimTrailingNL = [](std::wstring& s) {
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    };
    auto flush = [&](bool isCode) {
        if (isCode) trimTrailingNL(buf);
        if (!buf.empty()) {
            PaintSeg s;
            s.text = buf;
            s.isCode = isCode;
            if (isCode) {
                s.language = pendingLang;
                pendingLang.clear();
            }
            out.push_back(s);
            buf.clear();
        }
    };

    size_t i = 0;
    while (i < raw.size()) {
        if (i + 2 < raw.size() && raw[i] == L'`' && raw[i+1] == L'`' && raw[i+2] == L'`') {
            flush(inCode);
            i += 3;
            if (!inCode) {
                // Capture optional language label up to end of line
                pendingLang.clear();
                while (i < raw.size() && raw[i] != L'\n' && raw[i] != L'\r') {
                    pendingLang += raw[i];
                    i++;
                }
                if (i < raw.size() && raw[i] == L'\r') i++;
                if (i < raw.size() && raw[i] == L'\n') i++;
            }
            inCode = !inCode;
        } else {
            buf += raw[i++];
        }
    }
    flush(inCode);
    return out;
}

// Split a code block into lines (without the newlines)
static std::vector<std::wstring> SplitLines(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == L'\n') { out.push_back(cur); cur.clear(); }
        else if (c != L'\r') cur += c;
    }
    out.push_back(cur);
    return out;
}

void OverlayWindow::OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    auto theme = ConfigLoader::GetTheme(m_config.theme);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, theme.prose_text);

    HFONT hProseFont = CreateFont(
        m_config.font_size_prose, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT hProseBold = CreateFont(
        m_config.font_size_prose, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT hProseItalic = CreateFont(
        m_config.font_size_prose, 0, 0, 0, FW_NORMAL, TRUE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT hProseBoldIt = CreateFont(
        m_config.font_size_prose, 0, 0, 0, FW_BOLD, TRUE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
    HFONT hCodeFont = CreateFont(
        m_config.font_size_code, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hProseFont);

    HFONT proseFonts[4] = { hProseFont, hProseBold, hProseItalic, hProseBoldIt };

    HBRUSH hUserBrush = CreateSolidBrush(theme.user_bubble);
    HBRUSH hBotBrush  = CreateSolidBrush(theme.bot_bubble);
    HBRUSH hCodeBrush = CreateSolidBrush(theme.code_bg);
    HPEN   hNullPen   = CreatePen(PS_NULL, 0, 0);
    HPEN   hOldPen    = (HPEN)SelectObject(hdc, hNullPen);

    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    int width      = clientRect.right - clientRect.left;
    int fullHeight = clientRect.bottom - clientRect.top;
    const int barHeight = 50;
    int height     = fullHeight - barHeight;

    int bubblePadding = 10;
    int sideMargin    = 10;
    int innerPadX     = 10;
    int innerPadY     = 5;
    int segGap        = 4;
    int codeInsetX    = 4;
    int proseMaxInner = (int)(width * 0.85) - 2 * innerPadX;
    int bubbleMaxW    = width - 2 * sideMargin;   // hard cap: full overlay width

    // Measure code font cell size for monospace line-by-line layout
    SelectObject(hdc, hCodeFont);
    SIZE codeCh;
    GetTextExtentPoint32W(hdc, L"M", 1, &codeCh);
    int codeCharW = codeCh.cx;
    TEXTMETRIC codeTm;
    GetTextMetrics(hdc, &codeTm);
    int codeLineH = codeTm.tmHeight + 2;

    // Measure prose line height for the styled-markdown layout
    SelectObject(hdc, hProseFont);
    TEXTMETRIC proseTm;
    GetTextMetrics(hdc, &proseTm);
    int proseLineH = proseTm.tmHeight + 2;

    // -------- Measure pass --------
    struct MeasuredMsg {
        std::vector<PaintSeg>            segs;
        std::vector<int>                 segHeights;
        std::vector<std::vector<std::wstring>>   codeLines;   // empty for prose segs
        std::vector<std::vector<COLORREF>>       codeColors;  // empty for prose segs
        int innerW;
        int bubbleW;
        int bubbleH;
    };
    std::vector<MeasuredMsg> measured;
    measured.reserve(m_messages.size());

    int totalY = 10;
    for (const auto& msg : m_messages) {
        MeasuredMsg mm;
        mm.segs = ParseSegments(msg.text);
        if (mm.segs.empty()) mm.segs.push_back({ L" ", false });

        mm.innerW = 0;
        int innerH = 0;
        for (size_t si = 0; si < mm.segs.size(); ++si) {
            const auto& seg = mm.segs[si];
            if (seg.isCode) {
                // Code: monospace, no word-wrap, line-by-line
                auto lines  = SplitLines(seg.text);
                auto colors = ColorizeBrackets(seg.text, seg.language);
                int maxLineChars = 0;
                for (const auto& ln : lines) {
                    if ((int)ln.size() > maxLineChars) maxLineChars = (int)ln.size();
                }
                int sw = std::min(bubbleMaxW - 2 * (innerPadX + codeInsetX),
                                  maxLineChars * codeCharW + 4);
                int sh = (int)lines.size() * codeLineH;
                mm.segHeights.push_back(sh);
                if (sw > mm.innerW) mm.innerW = sw;
                innerH += sh;
                mm.codeLines.push_back(std::move(lines));
                mm.codeColors.push_back(std::move(colors));
            } else {
                // Prose: prefer styled-markdown layout when markers are present;
                // fall back to fast DrawText for plain text.
                int sw, sh;
                if (HasInlineMd(seg.text)) {
                    auto runs = ParseMdRuns(seg.text);
                    auto res = LayoutMd(hdc, runs, 0, 0, proseMaxInner, proseLineH,
                                        proseFonts, RGB(255,255,255), false);
                    sw = res.width;
                    sh = res.height;
                } else {
                    SelectObject(hdc, hProseFont);
                    RECT r = { 0, 0, proseMaxInner, 0 };
                    DrawText(hdc, seg.text.c_str(), -1, &r,
                             DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                    sw = r.right - r.left;
                    sh = r.bottom - r.top;
                }
                mm.segHeights.push_back(sh);
                if (sw > mm.innerW) mm.innerW = sw;
                innerH += sh;
                mm.codeLines.push_back({});
                mm.codeColors.push_back({});
            }
            if (si + 1 < mm.segs.size()) innerH += segGap;
        }

        mm.bubbleW = std::min(mm.innerW + 2 * innerPadX, bubbleMaxW);
        mm.bubbleH = innerH + 2 * innerPadY;
        totalY += mm.bubbleH + bubblePadding;
        measured.push_back(std::move(mm));
    }

    m_contentHeight = totalY;

    int maxScroll = std::max(0, m_contentHeight - height + 20);
    if (m_scrollOffset > maxScroll) m_scrollOffset = maxScroll;
    if (m_scrollOffset < 0) m_scrollOffset = 0;

    // -------- Draw pass --------
    m_bubbleBounds.assign(m_messages.size(), RECT{0,0,0,0});

    int drawY = 10 - m_scrollOffset;
    for (size_t i = 0; i < m_messages.size(); ++i) {
        const auto& msg = m_messages[i];
        const auto& mm  = measured[i];

        if (drawY + mm.bubbleH > 0 && drawY < height) {
            int bubbleX = msg.isUser ? (width - mm.bubbleW - sideMargin) : sideMargin;

            // Record bounds for hit-testing in select mode
            m_bubbleBounds[i] = { bubbleX, drawY,
                                  bubbleX + mm.bubbleW, drawY + mm.bubbleH };

            HBRUSH bg = msg.isUser ? hUserBrush : hBotBrush;
            HBRUSH oldBg = (HBRUSH)SelectObject(hdc, bg);
            RoundRect(hdc, bubbleX, drawY,
                      bubbleX + mm.bubbleW, drawY + mm.bubbleH, 10, 10);
            SelectObject(hdc, oldBg);

            int segY = drawY + innerPadY;
            int segLeft  = bubbleX + innerPadX;
            int segRight = bubbleX + mm.bubbleW - innerPadX;

            for (size_t si = 0; si < mm.segs.size(); ++si) {
                const auto& seg = mm.segs[si];
                int sh = mm.segHeights[si];

                if (seg.isCode) {
                    // Inset darker block
                    HBRUSH oldB = (HBRUSH)SelectObject(hdc, hCodeBrush);
                    RoundRect(hdc,
                        segLeft - codeInsetX, segY - 2,
                        segRight + codeInsetX, segY + sh + 2,
                        4, 4);
                    SelectObject(hdc, oldB);

                    // Language chip in top-right corner of code block
                    if (!seg.language.empty()) {
                        HFONT hLangFont = CreateFont(
                            10, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                        SelectObject(hdc, hLangFont);
                        SetTextColor(hdc, RGB(120, 200, 255));
                        RECT lr = { segLeft, segY - 1, segRight + codeInsetX - 4, segY + 12 };
                        DrawText(hdc, seg.language.c_str(), -1, &lr,
                                 DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
                        DeleteObject(hLangFont);
                    }

                    SelectObject(hdc, hCodeFont);

                    // Clip to bubble interior so long lines don't draw past the bubble
                    HRGN clip = CreateRectRgn(segLeft, segY, segRight + codeInsetX, segY + sh + 2);
                    SelectClipRgn(hdc, clip);

                    const auto& lines  = mm.codeLines[si];
                    const auto& colors = mm.codeColors[si];

                    // We need to know each char's absolute index in the source for color lookup.
                    // Source 'seg.text' may differ from concatenated 'lines' only by removed CR/LF.
                    // So we walk seg.text in parallel and recompute per-line color indices.
                    size_t srcIdx = 0;
                    int    lineY  = segY;
                    for (const auto& ln : lines) {
                        // Find this line's char indices in seg.text (skipping consumed \r\n we removed)
                        std::vector<size_t> idxs;
                        idxs.reserve(ln.size());
                        size_t lnIdx = 0;
                        while (srcIdx < seg.text.size() && lnIdx < ln.size()) {
                            wchar_t sc = seg.text[srcIdx];
                            if (sc == L'\r') { srcIdx++; continue; }
                            if (sc == L'\n') break;  // shouldn't happen mid-line
                            idxs.push_back(srcIdx);
                            srcIdx++;
                            lnIdx++;
                        }
                        // Consume trailing \r\n separator
                        if (srcIdx < seg.text.size() && seg.text[srcIdx] == L'\r') srcIdx++;
                        if (srcIdx < seg.text.size() && seg.text[srcIdx] == L'\n') srcIdx++;

                        // Draw the line as runs of same color (apply horizontal scroll offset)
                        size_t j = 0;
                        int x = segLeft - m_codeScrollX;
                        while (j < ln.size()) {
                            COLORREF c = idxs[j] < colors.size() ? colors[idxs[j]] : kDefaultCodeColor;
                            size_t e = j + 1;
                            while (e < ln.size()) {
                                COLORREF nc = idxs[e] < colors.size() ? colors[idxs[e]] : kDefaultCodeColor;
                                if (nc != c) break;
                                e++;
                            }
                            SetTextColor(hdc, c);
                            int n = (int)(e - j);
                            TextOutW(hdc, x, lineY, ln.data() + j, n);
                            x += n * codeCharW;
                            j = e;
                        }

                        lineY += codeLineH;
                    }

                    SelectClipRgn(hdc, NULL);
                    DeleteObject(clip);
                } else {
                    if (HasInlineMd(seg.text)) {
                        auto runs = ParseMdRuns(seg.text);
                        LayoutMd(hdc, runs, segLeft, segY, segRight - segLeft, proseLineH,
                                 proseFonts, theme.prose_text, true);
                    } else {
                        SelectObject(hdc, hProseFont);
                        SetTextColor(hdc, theme.prose_text);
                        RECT tr = { segLeft, segY, segRight, segY + sh };
                        DrawText(hdc, seg.text.c_str(), -1, &tr,
                                 DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
                    }
                }

                segY += sh + segGap;
            }
            SetTextColor(hdc, theme.prose_text);

            // Timestamp in top-right corner of the bubble (if enabled and known)
            if (m_config.show_timestamps && msg.hour >= 0) {
                HFONT hTimeFont = CreateFont(
                    10, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
                SelectObject(hdc, hTimeFont);
                SetTextColor(hdc, RGB(160, 160, 160));
                wchar_t timeBuf[16];
                wsprintfW(timeBuf, L"%02d:%02d", msg.hour, msg.minute);
                RECT tr = { bubbleX + mm.bubbleW - 42, drawY - 12, bubbleX + mm.bubbleW - 2, drawY + 2 };
                DrawText(hdc, timeBuf, -1, &tr,
                         DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
                DeleteObject(hTimeFont);
            }
        }
        drawY += mm.bubbleH + bubblePadding;
    }

    // -------- Search bar (top, when active) --------
    if (m_searchActive) {
        RECT srcRect = { 0, 0, width, 26 };
        HBRUSH hSb = CreateSolidBrush(RGB(30, 100, 60));
        FillRect(hdc, &srcRect, hSb);
        DeleteObject(hSb);
        HFONT hSearchFont = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        SelectObject(hdc, hSearchFont);
        SetTextColor(hdc, RGB(255, 255, 255));
        std::wstring shown = L"Search: " + m_searchQuery + L"_  (Enter = jump, Esc = close)";
        RECT srTr = { 10, 4, width - 10, 22 };
        DrawText(hdc, shown.c_str(), -1, &srTr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        DeleteObject(hSearchFont);
    }

    // -------- Hotkey hints overlay (F2) --------
    if (m_hintsVisible) {
        const int panelW = std::min(width - 40, 320);
        const int panelH = std::min(fullHeight - 80, 360);
        int px = (width - panelW) / 2;
        int py = (fullHeight - panelH) / 2;

        HBRUSH hPanelBg = CreateSolidBrush(RGB(15, 15, 20));
        HBRUSH oldB = (HBRUSH)SelectObject(hdc, hPanelBg);
        RoundRect(hdc, px, py, px + panelW, py + panelH, 8, 8);
        SelectObject(hdc, oldB);
        DeleteObject(hPanelBg);

        HFONT hHdr = CreateFont(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        SelectObject(hdc, hHdr);
        SetTextColor(hdc, RGB(120, 200, 255));
        RECT hr = { px + 14, py + 10, px + panelW - 10, py + 30 };
        DrawText(hdc, L"Hotkeys (F2 to close)", -1, &hr,
                 DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        DeleteObject(hHdr);

        HFONT hRow = CreateFont(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        SelectObject(hdc, hRow);
        SetTextColor(hdc, RGB(220, 220, 220));

        int ly = py + 36;
        for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
            std::wstring line = std::wstring(L"  ");
            std::string b = ConfigLoader::BindingToString(m_config.hotkeys.bindings[i]);
            std::string lbl = ConfigLoader::ActionLabel((HotkeyAction)i);
            // pad binding to 14 chars
            while ((int)b.size() < 14) b += ' ';
            std::string row = "  " + b + lbl;
            int sz = MultiByteToWideChar(CP_UTF8, 0, row.data(), (int)row.size(), NULL, 0);
            std::wstring wrow(sz, 0);
            MultiByteToWideChar(CP_UTF8, 0, row.data(), (int)row.size(), &wrow[0], sz);
            RECT lr = { px + 14, ly, px + panelW - 10, ly + 16 };
            DrawText(hdc, wrow.c_str(), -1, &lr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            ly += 16;
        }
        // Fixed shortcuts
        const wchar_t* fixed[] = {
            L"  F1            About",
            L"  F2            This panel",
            L"  F11           Runtime settings",
            L"  Ctrl+E        Export chat to .md",
            L"  Ctrl+F        Search chat",
            L"  Ctrl+= / -    Font size",
            L"  Ctrl+Shift+R  Regenerate last answer",
            L"  Shift+← / →   Scroll code horizontally",
        };
        ly += 4;
        for (auto* l : fixed) {
            RECT lr = { px + 14, ly, px + panelW - 10, ly + 16 };
            DrawText(hdc, l, -1, &lr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            ly += 16;
        }
        DeleteObject(hRow);
    }

    // -------- Mode banner (move OR select) --------
    if (m_moveMode || m_selectMode) {
        RECT banner = { 0, 0, width, 22 };
        COLORREF bgColor = m_selectMode ? RGB(40, 120, 200) : RGB(180, 80, 30);
        HBRUSH hBan = CreateSolidBrush(bgColor);
        FillRect(hdc, &banner, hBan);
        DeleteObject(hBan);

        HFONT hBanFont = CreateFont(
            14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        SelectObject(hdc, hBanFont);
        SetTextColor(hdc, RGB(255, 255, 255));
        const wchar_t* text = m_selectMode
            ? L"SELECT MODE — click any message to copy it. Esc to cancel."
            : L"MOVE MODE — drag to move, edges to resize, F10 to lock";
        DrawText(hdc, text, -1, &banner, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        DeleteObject(hBanFont);
    }

    // -------- Live transcript bar --------
    {
        RECT barRect = { 0, fullHeight - barHeight, width, fullHeight };

        HBRUSH hBarBg = CreateSolidBrush(theme.bar_bg);
        FillRect(hdc, &barRect, hBarBg);
        DeleteObject(hBarBg);

        HPEN hLinePen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
        HPEN oldLinePen = (HPEN)SelectObject(hdc, hLinePen);
        MoveToEx(hdc, 0, barRect.top, NULL);
        LineTo(hdc, width, barRect.top);
        SelectObject(hdc, oldLinePen);
        DeleteObject(hLinePen);

        // Audio level dot — left side, brightens with energy
        {
            int dotR = 5;
            int dotX = 12;
            int dotY = barRect.top + 12;
            // Map energy to brightness (clip energy at 0.05 = full green)
            float bright = std::min(1.0f, m_audioLevel / 0.05f);
            int g = 80 + (int)(bright * 175);
            HBRUSH dot = CreateSolidBrush(RGB(40, g, 40));
            HBRUSH oldDot = (HBRUSH)SelectObject(hdc, dot);
            Ellipse(hdc, dotX - dotR, dotY - dotR, dotX + dotR, dotY + dotR);
            SelectObject(hdc, oldDot);
            DeleteObject(dot);
        }

        // In-flight indicator: solid orange dot next to the audio dot
        if (m_inflightCalls.load() > 0) {
            int dotR = 5;
            int dotX = 28;
            int dotY = barRect.top + 12;
            HBRUSH dot = CreateSolidBrush(RGB(240, 160, 40));
            HBRUSH oldDot = (HBRUSH)SelectObject(hdc, dot);
            Ellipse(hdc, dotX - dotR, dotY - dotR, dotX + dotR, dotY + dotR);
            SelectObject(hdc, oldDot);
            DeleteObject(dot);
        }

        // AUTO badge (right)
        HFONT hBadgeFont = CreateFont(
            13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        SelectObject(hdc, hBadgeFont);
        const wchar_t* badge = m_autoMode ? L"AUTO ON" : L"AUTO OFF";
        SetTextColor(hdc, m_autoMode ? RGB(120, 230, 120) : RGB(140, 140, 140));
        RECT badgeRect = { width - 80, barRect.top + 4, width - 6, barRect.top + 20 };
        DrawText(hdc, badge, -1, &badgeRect, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
        DeleteObject(hBadgeFont);

        // Token counter (right side, before AUTO badge)
        if (m_tokensIn > 0 || m_tokensOut > 0) {
            HFONT hTokFont = CreateFont(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
            SelectObject(hdc, hTokFont);
            SetTextColor(hdc, RGB(140, 140, 140));
            wchar_t tokBuf[64];
            wsprintfW(tokBuf, L"%lldk in / %lldk out",
                      (long long)(m_tokensIn / 1000), (long long)(m_tokensOut / 1000));
            RECT tokRect = { width - 200, barRect.top + 22, width - 90, barRect.bottom - 4 };
            DrawText(hdc, tokBuf, -1, &tokRect, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
            DeleteObject(hTokFont);
        }

        // Transcript text — left padding includes room for the dot
        HFONT hTransFont = CreateFont(
            14, 0, 0, 0, FW_NORMAL, TRUE /*italic*/, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");
        SelectObject(hdc, hTransFont);
        SetTextColor(hdc, theme.bar_text);

        std::wstring shown = m_lastTranscript.empty() ? std::wstring(L"(press F9 to enable auto-answer)") : m_lastTranscript;
        RECT tRect = { 44, barRect.top + 4, width - 90, barRect.bottom - 4 };
        DrawText(hdc, shown.c_str(), -1, &tRect,
                 DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        DeleteObject(hTransFont);
    }

    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldFont);
    DeleteObject(hUserBrush);
    DeleteObject(hBotBrush);
    DeleteObject(hCodeBrush);
    DeleteObject(hNullPen);
    DeleteObject(hProseFont);
    DeleteObject(hProseBold);
    DeleteObject(hProseItalic);
    DeleteObject(hProseBoldIt);
    DeleteObject(hCodeFont);

    EndPaint(hwnd, &ps);
}
#endif  // end of moved-out rendering block
