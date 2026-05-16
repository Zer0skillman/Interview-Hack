#include "LLMClient.h"
#include "HttpClient.h"
#include <sstream>
#include <cstdio>
#include <cstdint>
#include <cwctype>
#include <functional>

static const wchar_t* SYSTEM_PROMPT =
    L"You are a concise AI assistant inside a desktop overlay used during live coding interviews "
    L"and study sessions. Answer directly. For coding problems: give working code first inside a "
    L"```code block```, then a one-line why. When the user attaches a screenshot, focus on the "
    L"question, code, or problem visible on screen. When the user attaches audio, it is the last "
    L"~30 seconds of the meeting they are in; the interviewer's question is likely in there — "
    L"transcribe internally, identify the question, and answer it. Be brief unless the question "
    L"demands depth.";

bool LLMClient::ProviderSupportsAudio(const std::string& provider) {
    return provider == "gemini" || provider.empty();
}

static const wchar_t* CLASSIFY_SYSTEM_PROMPT =
    L"You are an audio classifier for an interview-assist tool. The user attaches a few seconds "
    L"of a meeting they are in. Output EXACTLY this format and nothing else:\n"
    L"TRANSCRIPT: <one-line transcript of what was said>\n"
    L"QUESTION: <the substantive technical question that was asked, rephrased as a clean prompt>\n"
    L"\n"
    L"The QUESTION line is ONLY emitted when the audio contains a substantive technical question "
    L"that an interviewee would benefit from an answer to — for example: an interviewer asking the "
    L"user to solve a coding problem, explain a concept, describe a data structure, give "
    L"time/space complexity, write code, or evaluate a trade-off.\n"
    L"OMIT the QUESTION line entirely (do not write it at all) when the audio is: greetings, small "
    L"talk, behavioral questions ('tell me about a time'), the user themselves talking, silence, "
    L"music, or anything subjective. If unsure, OMIT QUESTION.";

// -----------------------------------------------------------------------------
// Portable UTF-8 <-> wstring. The Windows version used MultiByteToWideChar /
// WideCharToMultiByte; this is the same conversion in plain C++ so the rest
// of the code keeps wstring-based prompts unchanged.
// -----------------------------------------------------------------------------

std::string LLMClient::WideToUtf8(const std::wstring& wstr) {
    std::string out;
    out.reserve(wstr.size() * 2);
    for (size_t i = 0; i < wstr.size(); ++i) {
        uint32_t cp = (uint32_t)wstr[i];
        // Handle surrogate pairs (sizeof(wchar_t) == 2 on Windows)
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < wstr.size()) {
            uint32_t low = (uint32_t)wstr[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::wstring LLMClient::Utf8ToWide(const std::string& str) {
    std::wstring out;
    out.reserve(str.size());
    size_t i = 0;
    while (i < str.size()) {
        unsigned char b = (unsigned char)str[i];
        uint32_t cp = 0;
        int extra = 0;
        if (b < 0x80) { cp = b; extra = 0; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; extra = 1; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; extra = 2; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; extra = 3; }
        else { i++; continue; }
        ++i;
        for (int k = 0; k < extra; ++k) {
            if (i >= str.size()) { cp = 0; break; }
            cp = (cp << 6) | ((unsigned char)str[i] & 0x3F);
            ++i;
        }
        if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
            cp -= 0x10000;
            out += (wchar_t)(0xD800 + (cp >> 10));
            out += (wchar_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (wchar_t)cp;
        }
    }
    return out;
}

std::string LLMClient::JsonEscapeUtf8(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size() + 16);
    for (unsigned char c : utf8) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
// JSON parsing helpers (string field extraction with escape handling)
// -----------------------------------------------------------------------------

std::wstring LLMClient::ExtractFirstTextField(const std::wstring& json) {
    const std::wstring needle = L"\"text\"";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::wstring::npos) {
        size_t i = pos + needle.length();
        while (i < json.length() && (json[i] == L' ' || json[i] == L'\t')) i++;
        if (i >= json.length() || json[i] != L':') { pos++; continue; }
        i++;
        while (i < json.length() && (json[i] == L' ' || json[i] == L'\t')) i++;
        if (i >= json.length() || json[i] != L'"') { pos++; continue; }
        i++;

        std::wstring result;
        bool escape = false;
        for (; i < json.length(); i++) {
            wchar_t c = json[i];
            if (escape) {
                if (c == L'n') result += L'\n';
                else if (c == L'r') result += L'\r';
                else if (c == L't') result += L'\t';
                else if (c == L'"') result += L'"';
                else if (c == L'\\') result += L'\\';
                else if (c == L'/') result += L'/';
                else if (c == L'u' && i + 4 < json.length()) {
                    try {
                        result += static_cast<wchar_t>(std::stoi(json.substr(i + 1, 4), nullptr, 16));
                        i += 4;
                    } catch (...) { result += L"\\u"; }
                }
                else result += c;
                escape = false;
            } else {
                if (c == L'\\') escape = true;
                else if (c == L'"') return result;
                else result += c;
            }
        }
        return result;
    }
    return L"";
}

std::wstring LLMClient::ExtractErrorMessage(const std::wstring& json) {
    size_t errPos = json.find(L"\"error\"");
    if (errPos == std::wstring::npos) return L"";

    size_t msgPos = json.find(L"\"message\"", errPos);
    if (msgPos == std::wstring::npos) return L"";

    size_t i = msgPos + 9;
    while (i < json.length() && (json[i] == L' ' || json[i] == L'\t')) i++;
    if (i >= json.length() || json[i] != L':') return L"";
    i++;
    while (i < json.length() && (json[i] == L' ' || json[i] == L'\t')) i++;
    if (i >= json.length() || json[i] != L'"') return L"";
    i++;

    std::wstring result;
    bool escape = false;
    for (; i < json.length(); i++) {
        wchar_t c = json[i];
        if (escape) {
            if (c == L'n') result += L'\n';
            else if (c == L'"') result += L'"';
            else if (c == L'\\') result += L'\\';
            else result += c;
            escape = false;
        } else {
            if (c == L'\\') escape = true;
            else if (c == L'"') break;
            else result += c;
        }
    }
    return result;
}

// Generic JSON field extractor — used for OpenAI-compatible "content" + "message"
static std::wstring ExtractJsonStringField(const std::wstring& json, const std::wstring& field) {
    std::wstring needle = L"\"" + field + L"\"";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::wstring::npos) {
        size_t i = pos + needle.length();
        while (i < json.length() && (json[i] == L' ' || json[i] == L'\t')) i++;
        if (i >= json.length() || json[i] != L':') { pos++; continue; }
        i++;
        while (i < json.length() && (json[i] == L' ' || json[i] == L'\t')) i++;
        if (i >= json.length() || json[i] != L'"') { pos++; continue; }
        i++;
        std::wstring result;
        bool escape = false;
        for (; i < json.length(); i++) {
            wchar_t c = json[i];
            if (escape) {
                if (c == L'n') result += L'\n';
                else if (c == L'r') result += L'\r';
                else if (c == L't') result += L'\t';
                else if (c == L'"') result += L'"';
                else if (c == L'\\') result += L'\\';
                else if (c == L'/') result += L'/';
                else if (c == L'u' && i + 4 < json.length()) {
                    try {
                        result += static_cast<wchar_t>(std::stoi(json.substr(i + 1, 4), nullptr, 16));
                        i += 4;
                    } catch (...) { result += L"\\u"; }
                }
                else result += c;
                escape = false;
            } else {
                if (c == L'\\') escape = true;
                else if (c == L'"') return result;
                else result += c;
            }
        }
        return result;
    }
    return L"";
}

// -----------------------------------------------------------------------------
// Common helpers
// -----------------------------------------------------------------------------

static std::wstring HttpStatusToMessage(int sc) {
    switch (sc) {
        case 400: return L"Bad request — check model name and payload.";
        case 401: return L"401 Unauthorized — check your API key.";
        case 403: return L"403 Forbidden — key valid but lacks permission.";
        case 404: return L"404 Not Found — model name probably wrong.";
        case 429: return L"429 Rate limited — slow down or upgrade plan.";
        case 500: return L"500 Server error — provider issue, retry shortly.";
        case 503: return L"503 Service unavailable — provider overloaded.";
        default: {
            wchar_t buf[64];
            std::swprintf(buf, sizeof(buf)/sizeof(buf[0]), L"HTTP %d — request failed.", sc);
            return std::wstring(buf);
        }
    }
}

static std::string DefaultBaseUrlFor(const std::string& provider, const std::string& configured) {
    if (provider == "openai")     return "https://api.openai.com/v1";
    if (provider == "groq")       return "https://api.groq.com/openai/v1";
    if (provider == "deepseek")   return "https://api.deepseek.com/v1";
    if (provider == "openrouter") return "https://openrouter.ai/api/v1";
    if (provider == "custom") {
        if (configured.empty()) return "https://api.openai.com/v1";
        if (configured.rfind("http://", 0) != 0 && configured.rfind("https://", 0) != 0) {
            return "https://" + configured;
        }
        return configured;
    }
    return "https://api.openai.com/v1";
}

// -----------------------------------------------------------------------------
// Body builders
// -----------------------------------------------------------------------------

std::string LLMClient::BuildGeminiRequestBody(
    const std::wstring& systemPrompt,
    const std::wstring& userMessage,
    const std::vector<LLMTurn>& history,
    const std::string& pngBase64,
    const std::string& wavBase64)
{
    std::ostringstream body;
    body << "{\"systemInstruction\":{\"parts\":[{\"text\":\"";
    body << JsonEscapeUtf8(WideToUtf8(systemPrompt));
    body << "\"}]},\"generationConfig\":{\"temperature\":0.7},\"contents\":[";

    bool first = true;
    for (const auto& turn : history) {
        if (!first) body << ",";
        first = false;
        body << "{\"role\":\"" << (turn.isUser ? "user" : "model")
             << "\",\"parts\":[{\"text\":\""
             << JsonEscapeUtf8(WideToUtf8(turn.text))
             << "\"}]}";
    }

    if (!first) body << ",";
    body << "{\"role\":\"user\",\"parts\":[{\"text\":\""
         << JsonEscapeUtf8(WideToUtf8(userMessage))
         << "\"}";
    if (!pngBase64.empty()) {
        body << ",{\"inline_data\":{\"mime_type\":\"image/png\",\"data\":\""
             << pngBase64
             << "\"}}";
    }
    if (!wavBase64.empty()) {
        body << ",{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\""
             << wavBase64
             << "\"}}";
    }
    body << "]}]}";
    return body.str();
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

LLMClient::ClassifyResult LLMClient::ClassifyAndTranscribe(
    const std::string& wavBase64,
    const LLMConfig& config)
{
    ClassifyResult result;
    if (wavBase64.empty()) return result;
    if (config.api_key.empty()) return result;
    if (config.provider != "gemini" && !config.provider.empty()) return result;

    std::ostringstream body;
    body << "{\"systemInstruction\":{\"parts\":[{\"text\":\""
         << JsonEscapeUtf8(WideToUtf8(CLASSIFY_SYSTEM_PROMPT))
         << "\"}]},\"generationConfig\":{\"temperature\":0.2},\"contents\":[{\"role\":\"user\","
         << "\"parts\":[{\"text\":\"Process the attached audio per your instructions.\"},"
         << "{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\""
         << wavBase64 << "\"}}]}]}";

    std::string url = "https://generativelanguage.googleapis.com/v1beta/models/"
                    + config.model + ":generateContent";
    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "x-goog-api-key: " + config.api_key,
    };

    HttpResponse r = HttpClient::Post(url, body.str(), headers);
    if (!r.errorMsg.empty()) return result;

    std::wstring response = Utf8ToWide(r.body);
    if (!ExtractErrorMessage(response).empty()) return result;

    std::wstring raw = ExtractFirstTextField(response);
    if (raw.empty()) return result;

    auto trim = [](std::wstring& s) {
        size_t i = 0;
        while (i < s.size() && (s[i] == L' ' || s[i] == L'\t' || s[i] == L'\n' || s[i] == L'\r')) i++;
        s.erase(0, i);
        while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    };

    size_t tPos = raw.find(L"TRANSCRIPT:");
    size_t qPos = raw.find(L"QUESTION:");
    if (tPos != std::wstring::npos) {
        size_t start = tPos + 11;
        size_t end   = (qPos != std::wstring::npos && qPos > tPos) ? qPos : raw.size();
        result.transcript = raw.substr(start, end - start);
        trim(result.transcript);
    }
    if (qPos != std::wstring::npos) {
        result.questionText = raw.substr(qPos + 9);
        trim(result.questionText);
    }
    return result;
}

std::wstring LLMClient::GenerateContent(
    const std::wstring& userMessage,
    const std::vector<LLMTurn>& history,
    const LLMConfig& config,
    const std::string& pngBase64,
    const std::string& wavBase64)
{
    std::wstring collected;
    GenerateContentStreaming(userMessage, history, config, pngBase64, wavBase64,
        [&](const std::wstring& chunk, bool) { collected += chunk; });
    return collected;
}

void LLMClient::GenerateContentStreaming(
    const std::wstring& userMessage,
    const std::vector<LLMTurn>& history,
    const LLMConfig& config,
    const std::string& pngBase64,
    const std::string& wavBase64,
    LLMStreamCallback onChunk)
{
    if (config.api_key.empty()) {
        onChunk(L"Error: API key is missing.", true);
        return;
    }

    const std::string& p = config.provider;
    if (p == "gemini" || p.empty()) {
        CallGeminiStreaming(userMessage, history, config, pngBase64, wavBase64, onChunk);
        return;
    }
    if (p == "anthropic") {
        CallAnthropicStreaming(userMessage, history, config, pngBase64, onChunk);
        return;
    }
    if (p == "openai" || p == "groq" || p == "deepseek" || p == "openrouter" || p == "custom") {
        CallOpenAICompatStreaming(userMessage, history, config, pngBase64, onChunk);
        return;
    }
    onChunk(L"Error: Unsupported provider '" + Utf8ToWide(p) + L"'.", true);
}

std::wstring LLMClient::CallGemini(
    const std::wstring& userMessage,
    const std::vector<LLMTurn>& history,
    const LLMConfig& config,
    const std::string& pngBase64,
    const std::string& wavBase64)
{
    std::string body = BuildGeminiRequestBody(SYSTEM_PROMPT, userMessage, history, pngBase64, wavBase64);
    std::string url = "https://generativelanguage.googleapis.com/v1beta/models/"
                    + config.model + ":generateContent";
    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "x-goog-api-key: " + config.api_key,
    };

    HttpResponse r = HttpClient::Post(url, body, headers);
    if (!r.errorMsg.empty()) return Utf8ToWide(r.errorMsg);

    std::wstring response = Utf8ToWide(r.body);
    std::wstring apiError = ExtractErrorMessage(response);
    if (!apiError.empty()) return L"API Error: " + apiError;

    std::wstring text = ExtractFirstTextField(response);
    if (text.empty()) return L"Error: Could not parse response.";
    return text;
}

std::wstring LLMClient::SendHttpsRequest(
    const std::wstring& host,
    const std::wstring& path,
    const std::string& jsonBody,
    const std::wstring& headers)
{
    // Legacy entrypoint — kept for compatibility but no longer used internally.
    std::string url = "https://" + WideToUtf8(host) + WideToUtf8(path);
    std::vector<std::string> hdrList;
    {
        std::string hs = WideToUtf8(headers);
        size_t start = 0;
        while (start < hs.size()) {
            size_t end = hs.find("\r\n", start);
            std::string h = hs.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!h.empty()) hdrList.push_back(h);
            if (end == std::string::npos) break;
            start = end + 2;
        }
    }
    HttpResponse r = HttpClient::Post(url, jsonBody, hdrList);
    if (!r.errorMsg.empty()) return Utf8ToWide(r.errorMsg);
    return Utf8ToWide(r.body);
}

// -----------------------------------------------------------------------------
// Streaming impls
// -----------------------------------------------------------------------------

void LLMClient::CallGeminiStreaming(
    const std::wstring& userMessage,
    const std::vector<LLMTurn>& history,
    const LLMConfig& config,
    const std::string& pngBase64,
    const std::string& wavBase64,
    LLMStreamCallback onChunk)
{
    std::string body = BuildGeminiRequestBody(SYSTEM_PROMPT, userMessage, history, pngBase64, wavBase64);
    std::string url = "https://generativelanguage.googleapis.com/v1beta/models/"
                    + config.model + ":streamGenerateContent?alt=sse";
    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "x-goog-api-key: " + config.api_key,
    };

    bool aborted = false;
    HttpResponse r = HttpClient::StreamPost(url, body, headers,
        [&](const std::string& payload) -> bool {
            std::wstring wjson = Utf8ToWide(payload);

            // Detect error wrapped in a stream event
            if (wjson.find(L"\"error\"") != std::wstring::npos) {
                std::wstring errMsg = ExtractJsonStringField(wjson, L"message");
                if (errMsg.empty()) errMsg = L"API error in stream";
                onChunk(L"API Error: " + errMsg, true);
                aborted = true;
                return false;
            }

            std::wstring chunkText = ExtractFirstTextField(wjson);
            if (!chunkText.empty()) onChunk(chunkText, false);
            return true;
        });

    if (aborted) return;

    if (!r.errorMsg.empty()) {
        onChunk(Utf8ToWide(r.errorMsg), true);
        return;
    }
    if (r.statusCode != 0 && (r.statusCode < 200 || r.statusCode >= 300)) {
        std::wstring msg = HttpStatusToMessage(r.statusCode);
        std::wstring apiMsg = ExtractJsonStringField(Utf8ToWide(r.body), L"message");
        if (!apiMsg.empty()) msg += L" Details: " + apiMsg;
        onChunk(L"API Error: " + msg, true);
        return;
    }
    onChunk(L"", true);
}

void LLMClient::CallOpenAICompatStreaming(
    const std::wstring& userMessage,
    const std::vector<LLMTurn>& history,
    const LLMConfig& config,
    const std::string& pngBase64,
    LLMStreamCallback onChunk)
{
    std::ostringstream body;
    body << "{\"model\":\"" << config.model << "\",\"stream\":true,\"messages\":[";
    body << "{\"role\":\"system\",\"content\":\""
         << JsonEscapeUtf8(WideToUtf8(SYSTEM_PROMPT)) << "\"}";

    for (const auto& turn : history) {
        body << ",{\"role\":\"" << (turn.isUser ? "user" : "assistant")
             << "\",\"content\":\""
             << JsonEscapeUtf8(WideToUtf8(turn.text))
             << "\"}";
    }

    body << ",{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\""
         << JsonEscapeUtf8(WideToUtf8(userMessage)) << "\"}";
    if (!pngBase64.empty()) {
        body << ",{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,"
             << pngBase64 << "\"}}";
    }
    body << "]}]}";

    std::string url = DefaultBaseUrlFor(config.provider, config.base_url) + "/chat/completions";
    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "Authorization: Bearer " + config.api_key,
    };

    bool aborted = false;
    HttpResponse r = HttpClient::StreamPost(url, body.str(), headers,
        [&](const std::string& payload) -> bool {
            std::wstring wjson = Utf8ToWide(payload);
            if (wjson.find(L"\"error\"") != std::wstring::npos) {
                std::wstring msg = ExtractJsonStringField(wjson, L"message");
                if (msg.empty()) msg = L"unknown error";
                onChunk(L"API Error: " + msg, true);
                aborted = true;
                return false;
            }
            std::wstring text = ExtractJsonStringField(wjson, L"content");
            if (!text.empty()) onChunk(text, false);
            return true;
        });

    if (aborted) return;
    if (!r.errorMsg.empty()) { onChunk(Utf8ToWide(r.errorMsg), true); return; }
    if (r.statusCode != 0 && (r.statusCode < 200 || r.statusCode >= 300)) {
        std::wstring msg = HttpStatusToMessage(r.statusCode);
        std::wstring apiMsg = ExtractJsonStringField(Utf8ToWide(r.body), L"message");
        if (!apiMsg.empty()) msg += L" Details: " + apiMsg;
        onChunk(L"API Error: " + msg, true);
        return;
    }
    onChunk(L"", true);
}

void LLMClient::CallAnthropicStreaming(
    const std::wstring& userMessage,
    const std::vector<LLMTurn>& history,
    const LLMConfig& config,
    const std::string& pngBase64,
    LLMStreamCallback onChunk)
{
    std::ostringstream body;
    body << "{\"model\":\"" << config.model
         << "\",\"stream\":true,\"max_tokens\":4096,\"system\":\""
         << JsonEscapeUtf8(WideToUtf8(SYSTEM_PROMPT)) << "\",\"messages\":[";

    bool first = true;
    for (const auto& turn : history) {
        if (!first) body << ",";
        first = false;
        body << "{\"role\":\"" << (turn.isUser ? "user" : "assistant")
             << "\",\"content\":\""
             << JsonEscapeUtf8(WideToUtf8(turn.text))
             << "\"}";
    }

    if (!first) body << ",";
    body << "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\""
         << JsonEscapeUtf8(WideToUtf8(userMessage)) << "\"}";
    if (!pngBase64.empty()) {
        body << ",{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/png\",\"data\":\""
             << pngBase64 << "\"}}";
    }
    body << "]}]}";

    std::string url = "https://api.anthropic.com/v1/messages";
    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "x-api-key: " + config.api_key,
        "anthropic-version: 2023-06-01",
    };

    bool aborted = false;
    HttpResponse r = HttpClient::StreamPost(url, body.str(), headers,
        [&](const std::string& payload) -> bool {
            std::wstring wjson = Utf8ToWide(payload);
            if (wjson.find(L"\"type\":\"error\"") != std::wstring::npos
                || wjson.find(L"\"error\"") != std::wstring::npos)
            {
                std::wstring msg = ExtractJsonStringField(wjson, L"message");
                if (!msg.empty()) {
                    onChunk(L"API Error: " + msg, true);
                    aborted = true;
                    return false;
                }
            }
            if (wjson.find(L"content_block_delta") != std::wstring::npos) {
                std::wstring text = ExtractJsonStringField(wjson, L"text");
                if (!text.empty()) onChunk(text, false);
            }
            return true;
        });

    if (aborted) return;
    if (!r.errorMsg.empty()) { onChunk(Utf8ToWide(r.errorMsg), true); return; }
    if (r.statusCode != 0 && (r.statusCode < 200 || r.statusCode >= 300)) {
        std::wstring msg = HttpStatusToMessage(r.statusCode);
        std::wstring apiMsg = ExtractJsonStringField(Utf8ToWide(r.body), L"message");
        if (!apiMsg.empty()) msg += L" Details: " + apiMsg;
        onChunk(L"API Error: " + msg, true);
        return;
    }
    onChunk(L"", true);
}

// -----------------------------------------------------------------------------
// Single-call merged classifier + answer (streaming)
// -----------------------------------------------------------------------------

static const wchar_t* CLASSIFY_AND_ANSWER_PROMPT =
    L"You receive a few seconds of audio from the user's meeting. Output EXACTLY this format:\n"
    L"TRANSCRIPT: <one-line transcript of the speech>\n"
    L"ANSWER: <answer | NONE>\n\n"
    L"The ANSWER content must be either:\n"
    L"- The literal word NONE if the audio is greetings, small talk, behavioral, the user "
    L"themselves talking, silence, music, or unclear.\n"
    L"- A complete, helpful answer ONLY if the audio contains a substantive technical question "
    L"that an interviewee would benefit from answering (coding problem, concept explanation, "
    L"complexity, tradeoffs).\n"
    L"For coding answers, give working code inside a ```code block``` then a one-line why.\n"
    L"Output nothing else. No preamble, no commentary.";

namespace {
struct MergedParser {
    enum State { LookingForTranscript, BufferingTranscript, LookingForAnswer,
                 CheckingNone, StreamingAnswer, Done };
    State state = LookingForTranscript;
    std::wstring acc;
    bool transcriptEmitted = false;
    std::function<void(const std::wstring&)> onTranscript;
    LLMStreamCallback onAnswerChunk;

    void Feed(const std::wstring& chunk) {
        acc += chunk;
        Process();
    }

    void Process() {
        while (state != Done) {
            if (state == LookingForTranscript) {
                size_t p = acc.find(L"TRANSCRIPT:");
                if (p == std::wstring::npos) {
                    if (acc.size() > 200) acc = acc.substr(acc.size() - 200);
                    return;
                }
                acc.erase(0, p + 11);
                state = BufferingTranscript;
            }
            if (state == BufferingTranscript) {
                size_t nl  = acc.find(L'\n');
                size_t ans = acc.find(L"ANSWER:");
                if (nl == std::wstring::npos && ans == std::wstring::npos) return;
                size_t end = std::wstring::npos;
                if (ans != std::wstring::npos) end = ans;
                if (nl != std::wstring::npos && (end == std::wstring::npos || nl < end)) end = nl;

                std::wstring t = acc.substr(0, end);
                while (!t.empty() && (t.front() == L' ' || t.front() == L'\t')) t.erase(0, 1);
                while (!t.empty() && (t.back()  == L'\n' || t.back()  == L'\r'
                                   || t.back()  == L' '  || t.back()  == L'\t')) t.pop_back();

                if (!transcriptEmitted && !t.empty()) {
                    onTranscript(t);
                    transcriptEmitted = true;
                }
                acc.erase(0, end);
                state = LookingForAnswer;
            }
            if (state == LookingForAnswer) {
                size_t p = acc.find(L"ANSWER:");
                if (p == std::wstring::npos) return;
                acc.erase(0, p + 7);
                while (!acc.empty() && (acc.front() == L' ' || acc.front() == L'\t')) acc.erase(0, 1);
                state = CheckingNone;
            }
            if (state == CheckingNone) {
                if (acc.size() < 5) return;
                if (acc.compare(0, 4, L"NONE") == 0 &&
                    (acc.size() == 4 || !iswalnum(acc[4]))) {
                    state = Done;
                    return;
                }
                onAnswerChunk(acc, false);
                acc.clear();
                state = StreamingAnswer;
            }
            if (state == StreamingAnswer) {
                if (!acc.empty()) {
                    onAnswerChunk(acc, false);
                    acc.clear();
                }
                return;
            }
        }
    }

    void Finalize() {
        if (state == StreamingAnswer && !acc.empty()) {
            onAnswerChunk(acc, false);
            acc.clear();
        }
        onAnswerChunk(L"", true);
    }
};
}  // namespace

void LLMClient::ClassifyAndAnswerStreaming(
    const std::string& wavBase64,
    const LLMConfig& config,
    std::function<void(const std::wstring&)> onTranscript,
    LLMStreamCallback onAnswerChunk)
{
    if (wavBase64.empty() || config.api_key.empty()) { onAnswerChunk(L"", true); return; }
    if (config.provider != "gemini" && !config.provider.empty()) { onAnswerChunk(L"", true); return; }

    MergedParser parser;
    parser.onTranscript = onTranscript;
    parser.onAnswerChunk = onAnswerChunk;

    std::vector<LLMTurn> empty;
    std::string body = BuildGeminiRequestBody(
        CLASSIFY_AND_ANSWER_PROMPT,
        L"Process the attached audio per your instructions.",
        empty, std::string(), wavBase64);

    std::string url = "https://generativelanguage.googleapis.com/v1beta/models/"
                    + config.model + ":streamGenerateContent?alt=sse";
    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "x-goog-api-key: " + config.api_key,
    };

    HttpClient::StreamPost(url, body, headers,
        [&](const std::string& payload) -> bool {
            std::wstring wjson = Utf8ToWide(payload);
            std::wstring text = ExtractFirstTextField(wjson);
            if (!text.empty()) parser.Feed(text);
            return true;
        });

    parser.Finalize();
}
