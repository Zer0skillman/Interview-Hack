#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include "ConfigLoader.h"
#include "AudioCapture.h"

// Custom message for when a full (non-streaming) LLM response is ready
#define WM_LLM_RESPONSE (WM_APP + 1)
// Custom message for a streaming chunk. wParam: 1 if final, 0 otherwise. lParam: std::wstring*
#define WM_LLM_CHUNK    (WM_APP + 2)
// Background poll: transcript + optional answer. lParam = std::pair<wstring,wstring>*
#define WM_POLL_RESULT  (WM_APP + 3)

struct ChatMessage {
    std::wstring text;
    bool isUser; // true = User, false = Bot
    int  hour = -1;     // -1 = unknown (loaded from file); 0..23 valid local hour
    int  minute = -1;

    ChatMessage() {
        SYSTEMTIME st; GetLocalTime(&st);
        hour = st.wHour;
        minute = st.wMinute;
    }
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
    void HandleLLMChunk(const std::wstring& chunk, bool isFinal);
    void HandlePollResult(const std::wstring& transcript, const std::wstring& questionText);
    void ToggleAutoMode();
    void ToggleMoveMode();
    void ToggleSelectMode();
    void UpdateClickThrough();   // sets WS_EX_TRANSPARENT based on m_moveMode/m_selectMode
    void FirePoll();
    void ResetConversation();
    void CopyLastAnswer();

    // Send paths
    void CaptureScreenOnly();    // F8 default
    void CaptureAudioOnly();     // F7 default

    void ExportChat();           // Ctrl+E
    void ToggleSearch();         // Ctrl+F
    void ShowAbout();            // F1
    void RegenerateLastAnswer(); // Ctrl+Shift+R
    void ToggleHotkeyHints();    // F2 — show/hide cheat sheet panel
    void CheckForUpdateAsync();  // fires HTTPS GET if update_check_url is set
    void SaveConversation();     // write chat.txt
    void LoadConversation();     // read chat.txt
public:
    void AddInputTokens(long long n)  { m_tokensIn += n; }
    void AddOutputTokens(long long n) { m_tokensOut += n; }
private:

    void RegisterConfigHotkeys();
    void RequestAutoScroll();    // snap to bottom only if user was already at the bottom
    void OpenRuntimeSettings();  // F11 — re-open welcome dialog mid-session

public:
    // Exposed so the file-scope DispatchAsk helper can call it
    void TrimHistory();          // enforce m_config.history_cap

private:
    void Scroll(int delta);
    void ToggleVisibility();

    // Returns base64-encoded PNG of the monitor under the cursor, or empty on failure.
    std::string CaptureScreenshotAsBase64Png();

    HWND m_hwnd;
    HINSTANCE m_hInstance;
    std::vector<ChatMessage> m_messages;
    LLMConfig m_config;
    AudioCapture m_audio;

    // Scrolling
    int m_scrollOffset;
    int m_contentHeight;
    int m_codeScrollX = 0;  // horizontal pixel offset for code blocks (Shift+arrows)

    // Real-time audio state
    std::wstring        m_lastTranscript;       // most recent transcript line for the bar
    std::wstring        m_lastAutoAnswerKey;    // dedupe key (lowercased question prefix)
    bool                m_autoMode = false;     // F9 toggles
    std::atomic<bool>   m_pollInFlight{false};  // skip overlapping polls
    std::atomic<int>    m_inflightCalls{0};     // count of any in-flight LLM call (manual or auto)
    bool                m_moveMode = false;     // F10 toggles drag/resize mode
    bool                m_selectMode = false;   // Ctrl+Shift+C — click bubble to copy
    float               m_audioLevel = 0.0f;    // 0..1 for the level dot
    bool                m_lastPollHadSpeech = false; // adaptive interval

    // Auto-scroll behavior — track whether user was at bottom before new content
    bool                m_wasAtBottom = true;

    // Hit-test bounds for each message, recomputed every OnPaint
    std::vector<RECT>   m_bubbleBounds;

    // Conversation search
    bool                m_searchActive = false;
    std::wstring        m_searchQuery;
    bool                m_hintsVisible = false;

    // Token meter
    long long           m_tokensIn = 0;
    long long           m_tokensOut = 0;

    static const wchar_t* CLASS_NAME;
    static const wchar_t* WINDOW_TITLE;
};
