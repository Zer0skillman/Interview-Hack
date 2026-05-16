#include "ConfigLoader.h"
#include <fstream>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#else
#include "WinCompat.h"
#endif
#include <cstring>
#include <cstdlib>
#include <algorithm>

LLMConfig ConfigLoader::LoadConfig(const std::string& filepath) {
    LLMConfig config;
    std::ifstream file(filepath);

    // Defaults
    config.provider = "gemini";
    config.model = "gemini-2.5-flash-lite";
    config.hotkeys = DefaultHotkeys();

    if (!file.is_open()) return config;

    std::string line;
    while (std::getline(file, line)) {
        size_t delimiterPos = line.find('=');
        if (delimiterPos == std::string::npos) continue;
        std::string key = line.substr(0, delimiterPos);
        std::string value = line.substr(delimiterPos + 1);

        if (key == "model")            config.model = value;
        else if (key == "api_key")     config.api_key = value;
        else if (key == "provider")    config.provider = value;
        else if (key == "base_url")    config.base_url = value;
        else if (key == "gemini_fallback_key") config.gemini_fallback_key = value;
        else if (key == "audio_device_id") config.audio_device_id = value;
        else if (key == "mic_device_id")   config.mic_device_id = value;
        else if (key == "capture_mic") config.capture_mic = (value == "1" || value == "true");
        else if (key == "history_cap") config.history_cap = std::max(20, std::atoi(value.c_str()));
        else if (key == "opacity_alpha") config.opacity_alpha = std::max(60, std::min(255, std::atoi(value.c_str())));
        else if (key == "font_size_prose") config.font_size_prose = std::max(10, std::min(36, std::atoi(value.c_str())));
        else if (key == "font_size_code")  config.font_size_code  = std::max(10, std::min(32, std::atoi(value.c_str())));
        else if (key == "show_timestamps") config.show_timestamps = (value == "1" || value == "true");
        else if (key == "theme") config.theme = value;
        else if (key == "restore_session") config.restore_session = (value == "1" || value == "true");
        else if (key == "session_name")    config.session_name = value;
        else if (key == "sound_on_auto")   config.sound_on_auto = (value == "1" || value == "true");
        else if (key == "update_check_url") config.update_check_url = value;
        else if (key == "win_x")       config.win_x = std::atoi(value.c_str());
        else if (key == "win_y")       config.win_y = std::atoi(value.c_str());
        else if (key == "win_w")       config.win_w = std::atoi(value.c_str());
        else if (key == "win_h")       config.win_h = std::atoi(value.c_str());
        else if (key.rfind("hotkey.", 0) == 0) {
            std::string actionId = key.substr(7);
            for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
                if (actionId == ActionId((HotkeyAction)i)) {
                    HotkeyBinding b = BindingFromString(value);
                    if (!b.empty()) config.hotkeys.bindings[i] = b;
                    break;
                }
            }
        }
    }

    // One-time migration: pre-2.4.3 defaults hijacked system shortcuts (Ctrl+C,
    // Ctrl+R, Ctrl+Shift+C) globally, which broke text copying / browser
    // refresh / DevTools everywhere. If the user still has those exact old
    // bindings, transparently swap them to the new Ctrl+Alt+* defaults.
    auto migrate = [&](HotkeyAction a, UINT oldMods, UINT oldVk, UINT newMods, UINT newVk) {
        auto& b = config.hotkeys.bindings[(int)a];
        if (b.modifiers == oldMods && b.vk == oldVk) {
            b.modifiers = newMods;
            b.vk = newVk;
        }
    };
    migrate(HotkeyAction::CopyAnswer, MOD_CONTROL,             'C', MOD_CONTROL | MOD_ALT, 'C');
    migrate(HotkeyAction::ResetChat,  MOD_CONTROL,             'R', MOD_CONTROL | MOD_ALT, 'R');
    migrate(HotkeyAction::SelectMode, MOD_CONTROL | MOD_SHIFT, 'C', MOD_CONTROL | MOD_ALT, 'S');

    return config;
}

void ConfigLoader::SaveConfig(const std::string& filepath, const LLMConfig& config) {
    std::ofstream file(filepath);
    if (!file.is_open()) return;
    file << "provider=" << config.provider << "\n";
    file << "model=" << config.model << "\n";
    file << "api_key=" << config.api_key << "\n";
    if (!config.base_url.empty()) {
        file << "base_url=" << config.base_url << "\n";
    }
    if (!config.gemini_fallback_key.empty()) {
        file << "gemini_fallback_key=" << config.gemini_fallback_key << "\n";
    }
    if (!config.audio_device_id.empty()) file << "audio_device_id=" << config.audio_device_id << "\n";
    if (!config.mic_device_id.empty())   file << "mic_device_id=" << config.mic_device_id << "\n";
    file << "capture_mic=" << (config.capture_mic ? "1" : "0") << "\n";
    file << "history_cap=" << config.history_cap << "\n";
    file << "opacity_alpha=" << config.opacity_alpha << "\n";
    file << "font_size_prose=" << config.font_size_prose << "\n";
    file << "font_size_code=" << config.font_size_code << "\n";
    file << "show_timestamps=" << (config.show_timestamps ? "1" : "0") << "\n";
    file << "theme=" << config.theme << "\n";
    file << "restore_session=" << (config.restore_session ? "1" : "0") << "\n";
    file << "session_name=" << config.session_name << "\n";
    file << "sound_on_auto=" << (config.sound_on_auto ? "1" : "0") << "\n";
    if (!config.update_check_url.empty()) file << "update_check_url=" << config.update_check_url << "\n";
    if (config.win_w > 0 && config.win_h > 0) {
        file << "win_x=" << config.win_x << "\n";
        file << "win_y=" << config.win_y << "\n";
        file << "win_w=" << config.win_w << "\n";
        file << "win_h=" << config.win_h << "\n";
    }
    for (int i = 0; i < (int)HotkeyAction::Count; ++i) {
        file << "hotkey." << ActionId((HotkeyAction)i) << "="
             << BindingToString(config.hotkeys.bindings[i]) << "\n";
    }
}

const std::vector<ProviderInfo>& ConfigLoader::BuiltinProviders() {
    static const std::vector<ProviderInfo> providers = {
        {
            "gemini", "Google Gemini", true,
            {
                {"gemini-2.5-flash-lite", "Gemini 2.5 Flash Lite (free, fast)"},
                {"gemini-2.5-flash",      "Gemini 2.5 Flash (free, balanced)"},
                {"gemini-2.5-pro",        "Gemini 2.5 Pro (paid)"},
            }
        },
        {
            "openai", "OpenAI", false,
            {
                {"gpt-4o",       "GPT-4o (best vision)"},
                {"gpt-4o-mini",  "GPT-4o mini (cheap)"},
                {"o1",           "o1 (reasoning)"},
                {"gpt-5",        "GPT-5"},
            }
        },
        {
            "anthropic", "Anthropic Claude", false,
            {
                {"claude-opus-4-7",           "Claude Opus 4.7 (best reasoning)"},
                {"claude-sonnet-4-6",         "Claude Sonnet 4.6 (balanced)"},
                {"claude-haiku-4-5-20251001", "Claude Haiku 4.5 (fast)"},
            }
        },
        {
            "groq", "Groq (fast inference)", false,
            {
                {"llama-3.3-70b-versatile",     "Llama 3.3 70B"},
                {"llama-3.1-8b-instant",        "Llama 3.1 8B (instant)"},
                {"mixtral-8x7b-32768",          "Mixtral 8x7B"},
            }
        },
        {
            "deepseek", "DeepSeek (cheap coding)", false,
            {
                {"deepseek-chat",     "DeepSeek Chat"},
                {"deepseek-reasoner", "DeepSeek Reasoner"},
            }
        },
        {
            "openrouter", "OpenRouter (aggregator)", false,
            {
                {"anthropic/claude-sonnet-4-6", "Claude Sonnet 4.6 via OpenRouter"},
                {"openai/gpt-4o",               "GPT-4o via OpenRouter"},
                {"google/gemini-2.5-flash",     "Gemini 2.5 Flash via OpenRouter"},
            }
        },
        {
            "custom", "Custom (OpenAI-compatible)", false,
            {
                {"", "Type your model name"},
            }
        },
    };
    return providers;
}

const ProviderInfo* ConfigLoader::FindProvider(const std::string& id) {
    for (const auto& p : BuiltinProviders()) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

// ---- Hotkey helpers ----

ConfigLoader::ThemeColors ConfigLoader::GetTheme(const std::string& id) {
    if (id == "light") {
        return {
            RGB(245, 245, 248), // bg
            RGB(0, 120, 215),   // user bubble (same blue, readable on light)
            RGB(225, 225, 232), // bot bubble
            RGB(235, 235, 240), // code bg
            RGB(20, 20, 24),    // prose text
            RGB(20, 80, 30),    // code text (dark green)
            RGB(220, 220, 226), // bar bg
            RGB(50, 50, 60)     // bar text
        };
    }
    if (id == "contrast") {
        return {
            RGB(0, 0, 0),
            RGB(0, 100, 255),
            RGB(40, 40, 40),
            RGB(0, 0, 0),
            RGB(255, 255, 255),
            RGB(255, 255, 100),
            RGB(0, 0, 0),
            RGB(255, 255, 255)
        };
    }
    // dark (default)
    return {
        RGB(30, 30, 30),    // bg
        RGB(0, 120, 215),   // user
        RGB(60, 60, 60),    // bot
        RGB(22, 22, 28),    // code bg
        RGB(255, 255, 255), // prose
        RGB(220, 220, 220), // code (overridden per-char by bracket colorizer)
        RGB(20, 20, 20),    // bar bg
        RGB(190, 190, 190)  // bar text
    };
}

HotkeyConfig ConfigLoader::DefaultHotkeys() {
    HotkeyConfig c;
    c.bindings[(int)HotkeyAction::SendScreen]       = { 0, VK_F8 };
    c.bindings[(int)HotkeyAction::SendAudio]        = { 0, VK_F7 };
    c.bindings[(int)HotkeyAction::ToggleAuto]       = { 0, VK_F9 };
    c.bindings[(int)HotkeyAction::MoveMode]         = { 0, VK_F10 };
    // Ctrl+R / Ctrl+C / Ctrl+Shift+C would hijack system shortcuts (refresh,
    // copy, DevTools) globally, so we use Ctrl+Alt combos which apps almost
    // never bind. Pick distinct letters so they don't collide with each other.
    c.bindings[(int)HotkeyAction::ResetChat]        = { MOD_CONTROL | MOD_ALT, 'R' };
    c.bindings[(int)HotkeyAction::CopyAnswer]       = { MOD_CONTROL | MOD_ALT, 'C' };
    c.bindings[(int)HotkeyAction::SelectMode]       = { MOD_CONTROL | MOD_ALT, 'S' };
    c.bindings[(int)HotkeyAction::SendText]         = { 0, VK_INSERT };
    c.bindings[(int)HotkeyAction::ToggleVisibility] = { 0, VK_DELETE };
    c.bindings[(int)HotkeyAction::ExitApp]          = { 0, VK_END };
    c.bindings[(int)HotkeyAction::ScrollUp]         = { 0, VK_PRIOR };
    c.bindings[(int)HotkeyAction::ScrollDown]       = { 0, VK_NEXT };
    return c;
}

const char* ConfigLoader::ActionId(HotkeyAction a) {
    switch (a) {
        case HotkeyAction::SendScreen: return "send_screen";
        case HotkeyAction::SendAudio:  return "send_audio";
        case HotkeyAction::ToggleAuto: return "toggle_auto";
        case HotkeyAction::MoveMode:   return "move_mode";
        case HotkeyAction::ResetChat:  return "reset_chat";
        case HotkeyAction::CopyAnswer: return "copy_answer";
        case HotkeyAction::SelectMode:       return "select_mode";
        case HotkeyAction::SendText:         return "send_text";
        case HotkeyAction::ToggleVisibility: return "toggle_visibility";
        case HotkeyAction::ExitApp:          return "exit_app";
        case HotkeyAction::ScrollUp:         return "scroll_up";
        case HotkeyAction::ScrollDown:       return "scroll_down";
        default: return "unknown";
    }
}

const char* ConfigLoader::ActionLabel(HotkeyAction a) {
    switch (a) {
        case HotkeyAction::SendScreen:       return "Send screenshot";
        case HotkeyAction::SendAudio:        return "Send last 30s audio";
        case HotkeyAction::ToggleAuto:       return "Toggle auto-answer";
        case HotkeyAction::MoveMode:         return "Move / resize overlay";
        case HotkeyAction::ResetChat:        return "Reset conversation";
        case HotkeyAction::CopyAnswer:       return "Copy last AI answer";
        case HotkeyAction::SelectMode:       return "Select & copy a message";
        case HotkeyAction::SendText:         return "Send clipboard text";
        case HotkeyAction::ToggleVisibility: return "Show / hide overlay";
        case HotkeyAction::ExitApp:          return "Exit application";
        case HotkeyAction::ScrollUp:         return "Scroll chat up";
        case HotkeyAction::ScrollDown:       return "Scroll chat down";
        default: return "Unknown";
    }
}

std::string ConfigLoader::BindingToString(const HotkeyBinding& b) {
    if (b.empty()) return "(unset)";
    std::string out;
    if (b.modifiers & MOD_CONTROL) out += "Ctrl+";
    if (b.modifiers & MOD_ALT)     out += "Alt+";
    if (b.modifiers & MOD_SHIFT)   out += "Shift+";
    if (b.modifiers & MOD_WIN)     out += "Win+";

    // Friendly key names for common VKs
    switch (b.vk) {
        case VK_F1:  out += "F1"; break;
        case VK_F2:  out += "F2"; break;
        case VK_F3:  out += "F3"; break;
        case VK_F4:  out += "F4"; break;
        case VK_F5:  out += "F5"; break;
        case VK_F6:  out += "F6"; break;
        case VK_F7:  out += "F7"; break;
        case VK_F8:  out += "F8"; break;
        case VK_F9:  out += "F9"; break;
        case VK_F10: out += "F10"; break;
        case VK_F11: out += "F11"; break;
        case VK_F12: out += "F12"; break;
        case VK_INSERT: out += "Insert"; break;
        case VK_DELETE: out += "Delete"; break;
        case VK_HOME:   out += "Home"; break;
        case VK_END:    out += "End"; break;
        case VK_PRIOR:  out += "PageUp"; break;
        case VK_NEXT:   out += "PageDown"; break;
        case VK_SPACE:  out += "Space"; break;
        case VK_TAB:    out += "Tab"; break;
        case VK_RETURN: out += "Enter"; break;
        case VK_BACK:   out += "Backspace"; break;
        case VK_ESCAPE: out += "Esc"; break;
        default:
            if ((b.vk >= 'A' && b.vk <= 'Z') || (b.vk >= '0' && b.vk <= '9')) {
                out += (char)b.vk;
            } else {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "VK(%u)", b.vk);
                out += buf;
            }
    }
    return out;
}

HotkeyBinding ConfigLoader::BindingFromString(const std::string& s) {
    HotkeyBinding b;
    if (s.empty() || s == "(unset)") return b;
    std::string rest = s;
    auto consume = [&](const std::string& tok) {
        if (rest.size() >= tok.size() && rest.compare(0, tok.size(), tok) == 0) {
            rest.erase(0, tok.size());
            return true;
        }
        return false;
    };
    while (true) {
        if (consume("Ctrl+"))  { b.modifiers |= MOD_CONTROL; continue; }
        if (consume("Alt+"))   { b.modifiers |= MOD_ALT;     continue; }
        if (consume("Shift+")) { b.modifiers |= MOD_SHIFT;   continue; }
        if (consume("Win+"))   { b.modifiers |= MOD_WIN;     continue; }
        break;
    }

    static const struct { const char* name; UINT vk; } kNamed[] = {
        {"F1",VK_F1},{"F2",VK_F2},{"F3",VK_F3},{"F4",VK_F4},{"F5",VK_F5},{"F6",VK_F6},
        {"F7",VK_F7},{"F8",VK_F8},{"F9",VK_F9},{"F10",VK_F10},{"F11",VK_F11},{"F12",VK_F12},
        {"Insert",VK_INSERT},{"Delete",VK_DELETE},{"Home",VK_HOME},{"End",VK_END},
        {"PageUp",VK_PRIOR},{"PageDown",VK_NEXT},{"Space",VK_SPACE},{"Tab",VK_TAB},
        {"Enter",VK_RETURN},{"Backspace",VK_BACK},{"Esc",VK_ESCAPE},
    };
    for (auto& n : kNamed) {
        if (rest == n.name) { b.vk = n.vk; return b; }
    }
    if (rest.size() == 1) {
        char c = rest[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            b.vk = (UINT)c;
            return b;
        }
    }
    return b;  // unset on parse failure
}

std::vector<ModelInfo> ConfigLoader::LoadModels(const std::string& filepath) {
    std::vector<ModelInfo> models;
    std::ifstream file(filepath);
    
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty()) {
                std::stringstream ss(line);
                std::string id, name;
                ss >> id;
                std::getline(ss, name);
                
                if (!name.empty() && name[0] == ' ') name = name.substr(1);
                if (name.empty()) name = id;

                models.push_back({id, name});
            }
        }
    }

    // Fallback if file missing or empty
    if (models.empty()) {
        models = {
            {"gemini-2.5-flash-lite", "gemini-2.5-flash-lite (Free / Paid)"},
            {"gemini-2.5-flash", "gemini-2.5-flash (Free / Paid)"},
            {"gemini-2.5-flash-thinking-exp", "gemini-2.5-flash-thinking-exp (Free / Paid)"},
            {"gemini-3-flash-preview", "gemini-3-flash-preview (Free / Paid)"},
            {"gemini-3-pro-preview", "gemini-3-pro-preview (Paid)"},
            {"gemini-3-deepthink", "gemini-3-deepthink (Paid)"},
            {"gemini-2.5-pro", "gemini-2.5-pro (Paid)"}
        };
    }

    return models;
}
