#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include "ConfigLoader.h"

// Custom message for when LLM response is ready
#define WM_LLM_RESPONSE (WM_APP + 1)

struct ChatMessage {
    std::wstring text;
    bool isUser; // true = User, false = Bot
};

class OverlayWindow
{
public:
    OverlayWindow();
    ~OverlayWindow();

    bool Initialize(HINSTANCE hInstance);
    void RunMessageLoop();
    void SetConfig(const LLMConfig& config) { m_config = config; }
    
    // Callback helper
    void ReceiveLLMResponse(const std::wstring& response);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    void OnPaint(HWND hwnd);
    void UpdateFromClipboard();
    void HandleLLMResponse(const std::wstring& response);
    void Scroll(int delta);
    void ToggleVisibility();

    HWND m_hwnd;
    HINSTANCE m_hInstance;
    std::vector<ChatMessage> m_messages;
    LLMConfig m_config;
    
    // Scrolling
    int m_scrollOffset;
    int m_contentHeight;

    static const wchar_t* CLASS_NAME;
    static const wchar_t* WINDOW_TITLE;
};
