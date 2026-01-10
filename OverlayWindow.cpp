#include "OverlayWindow.h"
#include <algorithm> // for std::max

const wchar_t* OverlayWindow::CLASS_NAME = L"InvisibleOverlayClass";
const wchar_t* OverlayWindow::WINDOW_TITLE = L"Invisible Overlay";

OverlayWindow::OverlayWindow() : m_hwnd(NULL), m_hInstance(NULL), m_scrollOffset(0), m_contentHeight(0)
{
}

OverlayWindow::~OverlayWindow()
{
    if (m_hwnd)
    {
        UnregisterHotKey(m_hwnd, 1);
        UnregisterHotKey(m_hwnd, 2);
        UnregisterHotKey(m_hwnd, 3);
        UnregisterHotKey(m_hwnd, 4);
        UnregisterHotKey(m_hwnd, 5);
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
    
    // Sidebar: 15% width, start from top (with some padding)
    int winW = (int)(screenW * 0.15); 
    int winH = (int)(screenH * 0.70); // 70% height
    int x = screenW - winW - 20; // 20px padding from right edge
    int y = 100; // 100px padding from top

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

    // Hotkeys
    // 1: INS - Chat (Update)
    // 2: DEL - Toggle Visibility
    // 3: END - Exit
    // 4: PgUp - Scroll Up
    // 5: PgDn - Scroll Down
    RegisterHotKey(m_hwnd, 1, 0, VK_INSERT);
    RegisterHotKey(m_hwnd, 2, 0, VK_DELETE);
    RegisterHotKey(m_hwnd, 3, 0, VK_END);
    RegisterHotKey(m_hwnd, 4, 0, VK_PRIOR);
    RegisterHotKey(m_hwnd, 5, 0, VK_NEXT);

    #ifndef WDA_EXCLUDEFROMCAPTURE
    #define WDA_EXCLUDEFROMCAPTURE 0x00000011
    #endif

    if (!SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE))
    {
        SetWindowDisplayAffinity(m_hwnd, WDA_MONITOR);
    }

    SetLayeredWindowAttributes(m_hwnd, 0, 210, LWA_ALPHA);

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

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
            switch (wParam) {
                case 1: pThis->UpdateFromClipboard(); break; // Insert
                case 2: pThis->ToggleVisibility(); break;    // Delete
                case 3: PostQuitMessage(0); break;           // End
                case 4: pThis->Scroll(-50); break;           // PgUp
                case 5: pThis->Scroll(50); break;            // PgDn
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

    case WM_PAINT:
        if (pThis)
        {
            pThis->OnPaint(hwnd);
        }
        return 0;

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
    
    // Clamp Logic
    int maxScroll = std::max(0, m_contentHeight - windowHeight + 20); // +20 margin
    
    if (m_scrollOffset < 0) m_scrollOffset = 0;
    if (m_scrollOffset > maxScroll) m_scrollOffset = maxScroll;

    InvalidateRect(m_hwnd, NULL, TRUE);
}

#include "LLMClient.h"
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

            // Force repaint to calculate new height and auto-scroll
            // We set scroll to a very large number, OnPaint clamping will fix it to bottom
            m_scrollOffset = 999999;
            InvalidateRect(m_hwnd, NULL, TRUE);
            
            // Start Async Request
            LLMConfig cfg = m_config; // copy
            HWND hwnd = m_hwnd;

            std::thread([hwnd, clipText, cfg]() {
                std::wstring response = LLMClient::GenerateContent(clipText, cfg);
                
                // Pass back to main thread via Message
                // We allocate string on heap to pass it safely, main thread must delete it
                std::wstring* safeResponse = new std::wstring(response);
                PostMessage(hwnd, WM_LLM_RESPONSE, 0, (LPARAM)safeResponse);
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
        // Auto-scroll to bottom on update too
        m_scrollOffset = 999999; 
        InvalidateRect(m_hwnd, NULL, TRUE);
    }
}

void OverlayWindow::OnPaint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    // Styling Setup
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255)); // White Text

    HFONT hFont = CreateFont(
        18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_OUTLINE_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Segoe UI");

    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

    // Brushes for bubbles
    HBRUSH hUserBrush = CreateSolidBrush(RGB(0, 120, 215)); // Windows Blue
    HBRUSH hBotBrush = CreateSolidBrush(RGB(60, 60, 60));   // Darker Grey
    HPEN hNullPen = CreatePen(PS_NULL, 0, 0);               // No Border
    HPEN hOldPen = (HPEN)SelectObject(hdc, hNullPen);

    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    int width = clientRect.right - clientRect.left;
    int height = clientRect.bottom - clientRect.top;
    
    int bubblePadding = 10;
    int sideMargin = 10;
    int maxBubbleWidth = (int)(width * 0.85);

    // 1. Measure Pass (Calculate positions)
    // We need to know total height to clamp scroll
    int currentY = 10;
    std::vector<RECT> messageRects;
    std::vector<int> messageHeights;

    for (const auto& msg : m_messages)
    {
        RECT textRect = { 0, 0, maxBubbleWidth, 0 };
        DrawText(hdc, msg.text.c_str(), -1, &textRect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        
        int bubbleW = textRect.right - textRect.left + 20; 
        int bubbleH = textRect.bottom - textRect.top + 10; 
        
        messageRects.push_back({0, 0, bubbleW, bubbleH}); // Store W/H
        
        currentY += bubbleH + bubblePadding;
    }
    
    m_contentHeight = currentY;

    // 2. Clamp Scroll
    int maxScroll = std::max(0, m_contentHeight - height + 20);
    if (m_scrollOffset > maxScroll) m_scrollOffset = maxScroll;
    if (m_scrollOffset < 0) m_scrollOffset = 0;

    // 3. Draw Pass
    int drawY = 10 - m_scrollOffset;

    for (size_t i = 0; i < m_messages.size(); ++i)
    {
        const auto& msg = m_messages[i];
        int bubbleW = messageRects[i].right;
        int bubbleH = messageRects[i].bottom;

        // Optimization: Skip if off-screen
        if (drawY + bubbleH > 0 && drawY < height)
        {
            int bubbleX = msg.isUser ? (width - bubbleW - sideMargin) : sideMargin;
            
            RECT bubbleRect = { bubbleX, drawY, bubbleX + bubbleW, drawY + bubbleH };
            
            HBRUSH currentBrush = msg.isUser ? hUserBrush : hBotBrush;
            HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, currentBrush);
            
            RoundRect(hdc, bubbleRect.left, bubbleRect.top, bubbleRect.right, bubbleRect.bottom, 10, 10);
            SelectObject(hdc, oldBrush);

            RECT textRect = bubbleRect;
            textRect.left += 10;
            textRect.right -= 10;
            textRect.top += 5;
            
            DrawText(hdc, msg.text.c_str(), -1, &textRect, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        }

        drawY += bubbleH + bubblePadding;
    }

    // Cleanup
    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldFont);
    DeleteObject(hUserBrush);
    DeleteObject(hBotBrush);
    DeleteObject(hNullPen);
    DeleteObject(hFont);

    EndPaint(hwnd, &ps);
}
