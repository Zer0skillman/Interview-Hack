#pragma once
#include <string>
#include <map>
#include <vector>

struct HotkeyBinding {
    unsigned int modifiers = 0;  // MOD_CONTROL | MOD_ALT | MOD_SHIFT bits (Win32 RegisterHotKey form)
    unsigned int vk        = 0;  // VK_F8, 'R', etc.
    bool empty() const { return vk == 0; }
};

enum class HotkeyAction {
    SendScreen,      // F8 by default — screenshot only
    SendAudio,       // F7 by default — last 30s audio only
    ToggleAuto,      // F9
    MoveMode,        // F10
    ResetChat,       // Ctrl+R
    CopyAnswer,      // Ctrl+C
    SelectMode,      // Ctrl+Shift+C
    SendText,        // INS — send clipboard text
    ToggleVisibility,// DEL
    ExitApp,         // END
    ScrollUp,        // PgUp
    ScrollDown,      // PgDn
    Count
};

struct HotkeyConfig {
    HotkeyBinding bindings[(int)HotkeyAction::Count];
};

struct LLMConfig {
    std::string provider;   // "gemini", "openai", "anthropic", "groq", "deepseek", "openrouter", "custom"
    std::string model;
    std::string api_key;
    std::string base_url;             // only used for "custom"; ignored otherwise
    std::string gemini_fallback_key;  // optional: used for audio when primary provider isn't Gemini
    std::string audio_device_id;      // empty = default render device (system speakers)
    std::string mic_device_id;        // empty = default capture device; only used if capture_mic=true
    HotkeyConfig hotkeys;

    // Window position/size — 0,0,0,0 means "use default"
    int win_x = 0, win_y = 0, win_w = 0, win_h = 0;

    // Audio settings
    bool capture_mic = false;     // false = loopback only (default); true = also mix in mic
    int  history_cap = 200;       // max chat messages before oldest are trimmed

    // Appearance
    int  opacity_alpha = 210;     // 0..255 for the layered window (default 210)
    int  font_size_prose = 18;    // Segoe UI height for prose
    int  font_size_code  = 15;    // Consolas height for code
    bool show_timestamps = false; // show HH:MM on each bubble
    std::string theme = "dark";   // "dark" | "light" | "contrast"

    // Conversation persistence
    bool restore_session = true;  // load chat.txt on launch
    std::string session_name = "default";  // active session, persisted as chat.<name>.txt

    // Auto-answer extras
    bool sound_on_auto = false;   // MessageBeep when auto-answer fires
};

struct ModelInfo {
    std::string id;
    std::string name;
};

// Built-in provider metadata: human-readable name, default model list.
struct ProviderInfo {
    std::string id;       // canonical key
    std::string name;     // shown in dropdown
    bool supportsAudio;   // currently only Gemini does
    std::vector<ModelInfo> models;
};

class ConfigLoader {
public:
    static LLMConfig LoadConfig(const std::string& filepath);
    static void SaveConfig(const std::string& filepath, const LLMConfig& config);
    static std::vector<ModelInfo> LoadModels(const std::string& filepath);

    static const std::vector<ProviderInfo>& BuiltinProviders();
    static const ProviderInfo* FindProvider(const std::string& id);

    struct ThemeColors {
        unsigned long bg;          // window background
        unsigned long user_bubble; // user message bubble fill
        unsigned long bot_bubble;  // bot message bubble fill
        unsigned long code_bg;     // code block inset
        unsigned long prose_text;  // default text color in prose
        unsigned long code_text;   // default text color in code
        unsigned long bar_bg;      // transcript bar bg
        unsigned long bar_text;    // transcript bar text
    };
    static ThemeColors GetTheme(const std::string& id);

    // Hotkey helpers — used by both the loader and the rebind UI
    static HotkeyConfig DefaultHotkeys();
    static const char*  ActionId(HotkeyAction a);     // serializable identifier ("send_screen" etc.)
    static const char*  ActionLabel(HotkeyAction a);  // human-readable ("Screenshot ask")
    static std::string  BindingToString(const HotkeyBinding& b);   // "Ctrl+R", "F8", etc.
    static HotkeyBinding BindingFromString(const std::string& s);  // parse serialized form
};
