#include "LLMClient.h"
#include <windows.h>
#include <winhttp.h>
#include <sstream>
#include <cstdio>

#pragma comment(lib, "winhttp.lib")

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

    std::wstring path = L"/v1beta/models/" + Utf8ToWide(config.model) + L":generateContent";
    std::wstring headers = L"Content-Type: application/json\r\nx-goog-api-key: "
                         + Utf8ToWide(config.api_key);

    std::wstring response = SendHttpsRequest(
        L"generativelanguage.googleapis.com", path, body.str(), headers);

    if (response.length() >= 6 && response.substr(0, 6) == L"Error:") return result;
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
    // Non-streaming path is only used by the legacy entry; collect chunks.
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

std::wstring LLMClient::CallGemini(
    const std::wstring& userMessage,
    const std::vector<LLMTurn>& history,
    const LLMConfig& config,
    const std::string& pngBase64,
    const std::string& wavBase64)
{
    std::string body = BuildGeminiRequestBody(SYSTEM_PROMPT, userMessage, history, pngBase64, wavBase64);

    std::wstring path = L"/v1beta/models/" + Utf8ToWide(config.model) + L":generateContent";
    std::wstring headers = L"Content-Type: application/json\r\nx-goog-api-key: "
                         + Utf8ToWide(config.api_key);

    std::wstring response = SendHttpsRequest(
        L"generativelanguage.googleapis.com",
        path,
        body,
        headers);

    if (response.length() >= 6 && response.substr(0, 6) == L"Error:") {
        return response;
    }

    // API errors come back as JSON like {"error":{"message":"..."}}.
    // Check for that before treating the first "text" we find as the reply.
    std::wstring apiError = ExtractErrorMessage(response);
    if (!apiError.empty()) {
        return L"API Error: " + apiError;
    }

    std::wstring text = ExtractFirstTextField(response);
    if (text.empty()) {
        return L"Error: Could not parse response.";
    }
    return text;
}

std::wstring LLMClient::SendHttpsRequest(
    const std::wstring& host,
    const std::wstring& path,
    const std::string& jsonBody,
    const std::wstring& headers)
{
    HINTERNET hSession = WinHttpOpen(L"AIOverlay/2.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) return L"Error: WinHttpOpen failed";
    // resolve, connect, send, receive (ms). Receive is the most likely to hang on bad networks.
    WinHttpSetTimeouts(hSession, 5000, 10000, 30000, 60000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return L"Error: WinHttpConnect failed";
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return L"Error: WinHttpOpenRequest failed";
    }

    BOOL ok = WinHttpSendRequest(hRequest,
        headers.c_str(), (DWORD)headers.length(),
        (LPVOID)jsonBody.c_str(), (DWORD)jsonBody.length(),
        (DWORD)jsonBody.length(), 0);

    if (ok) ok = WinHttpReceiveResponse(hRequest, NULL);

    std::string responseData;
    if (ok) {
        DWORD dwSize = 0;
        DWORD dwDownloaded = 0;
        do {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
            if (dwSize == 0) break;

            std::vector<char> buf(dwSize + 1, 0);
            if (WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) {
                responseData.append(buf.data(), dwDownloaded);
            }
        } while (dwSize > 0);
    } else {
        DWORD err = GetLastError();
        wchar_t errBuf[64];
        wsprintfW(errBuf, L"%lu", err);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return L"Error: HTTPS request failed (code " + std::wstring(errBuf) + L")";
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return Utf8ToWide(responseData);
}

// Walks the JSON looking for the first "text" field with a string value, then
// extracts the string literal honoring backslash escapes.
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

std::string LLMClient::WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int sz = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string out(sz, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &out[0], sz, NULL, NULL);
    return out;
}

std::wstring LLMClient::Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int sz = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring out(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &out[0], sz);
    return out;
}

// Walks the byte buffer extracting complete SSE events. Each "data: {...}" line
// up to the next blank-line separator is one event. Removes consumed bytes from
// `buffer` and leaves any partial trailing event intact for the next read.
static void DrainSSEBuffer(std::string& buffer, LLMStreamCallback& onChunk, bool& aborted) {
    while (!aborted) {
        size_t nn = buffer.find("\n\n");
        size_t rnrn = buffer.find("\r\n\r\n");
        size_t end;
        size_t sepLen;
        if (nn == std::string::npos && rnrn == std::string::npos) break;
        if (nn == std::string::npos)      { end = rnrn; sepLen = 4; }
        else if (rnrn == std::string::npos) { end = nn;   sepLen = 2; }
        else if (rnrn < nn)               { end = rnrn; sepLen = 4; }
        else                              { end = nn;   sepLen = 2; }

        std::string event = buffer.substr(0, end);
        buffer.erase(0, end + sepLen);

        size_t dataPos = event.find("data:");
        if (dataPos == std::string::npos) continue;
        size_t i = dataPos + 5;
        if (i < event.size() && event[i] == ' ') i++;
        std::string json = event.substr(i);
        while (!json.empty() && (json.back() == '\n' || json.back() == '\r')) json.pop_back();
        if (json.empty()) continue;

        // Convert and try to extract text. Reuse ExtractFirstTextField via wstring.
        int sz = MultiByteToWideChar(CP_UTF8, 0, json.data(), (int)json.size(), NULL, 0);
        std::wstring wjson(sz, 0);
        MultiByteToWideChar(CP_UTF8, 0, json.data(), (int)json.size(), &wjson[0], sz);

        // Check for API error wrapped in stream
        size_t errPos = wjson.find(L"\"error\"");
        if (errPos != std::wstring::npos) {
            size_t msgPos = wjson.find(L"\"message\"", errPos);
            std::wstring errMsg = L"API error in stream";
            if (msgPos != std::wstring::npos) {
                // Crude extract — close enough; helper isn't accessible here cheaply
                size_t qStart = wjson.find(L'"', msgPos + 9);
                if (qStart != std::wstring::npos) {
                    qStart++;
                    size_t qEnd = wjson.find(L'"', qStart);
                    if (qEnd != std::wstring::npos) errMsg = wjson.substr(qStart, qEnd - qStart);
                }
            }
            onChunk(L"API Error: " + errMsg, true);
            aborted = true;
            return;
        }

        // Extract text from the chunk
        size_t pos = 0;
        const std::wstring needle = L"\"text\"";
        std::wstring chunkText;
        while ((pos = wjson.find(needle, pos)) != std::wstring::npos) {
            size_t j = pos + needle.length();
            while (j < wjson.length() && (wjson[j] == L' ' || wjson[j] == L'\t')) j++;
            if (j >= wjson.length() || wjson[j] != L':') { pos++; continue; }
            j++;
            while (j < wjson.length() && (wjson[j] == L' ' || wjson[j] == L'\t')) j++;
            if (j >= wjson.length() || wjson[j] != L'"') { pos++; continue; }
            j++;

            std::wstring result;
            bool escape = false;
            for (; j < wjson.length(); j++) {
                wchar_t c = wjson[j];
                if (escape) {
                    if (c == L'n') result += L'\n';
                    else if (c == L'r') result += L'\r';
                    else if (c == L't') result += L'\t';
                    else if (c == L'"') result += L'"';
                    else if (c == L'\\') result += L'\\';
                    else if (c == L'/') result += L'/';
                    else if (c == L'u' && j + 4 < wjson.length()) {
                        try {
                            result += static_cast<wchar_t>(std::stoi(wjson.substr(j + 1, 4), nullptr, 16));
                            j += 4;
                        } catch (...) { result += L"\\u"; }
                    }
                    else result += c;
                    escape = false;
                } else {
                    if (c == L'\\') escape = true;
                    else if (c == L'"') break;
                    else result += c;
                }
            }
            chunkText = result;
            break;
        }
        if (!chunkText.empty()) {
            onChunk(chunkText, false);
        }
    }
}

void LLMClient::CallGeminiStreaming(
    const std::wstring& userMessage,
    const std::vector<LLMTurn>& history,
    const LLMConfig& config,
    const std::string& pngBase64,
    const std::string& wavBase64,
    LLMStreamCallback onChunk)
{
    std::string body = BuildGeminiRequestBody(SYSTEM_PROMPT, userMessage, history, pngBase64, wavBase64);

    std::wstring path = L"/v1beta/models/" + Utf8ToWide(config.model)
                      + L":streamGenerateContent?alt=sse";
    std::wstring headers = L"Content-Type: application/json\r\nx-goog-api-key: "
                         + Utf8ToWide(config.api_key);

    HINTERNET hSession = WinHttpOpen(L"AIOverlay/2.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) { onChunk(L"Error: WinHttpOpen failed", true); return; }
    // Streaming receive timeout is per-read; 60s is enough for slow tokens
    WinHttpSetTimeouts(hSession, 5000, 10000, 30000, 60000);

    HINTERNET hConnect = WinHttpConnect(hSession,
        L"generativelanguage.googleapis.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        onChunk(L"Error: WinHttpConnect failed", true);
        return;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        onChunk(L"Error: WinHttpOpenRequest failed", true);
        return;
    }

    BOOL ok = WinHttpSendRequest(hRequest,
        headers.c_str(), (DWORD)headers.length(),
        (LPVOID)body.c_str(), (DWORD)body.length(),
        (DWORD)body.length(), 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, NULL);

    if (!ok) {
        DWORD err = GetLastError();
        std::wstring msg = L"network request failed";
        if (err == ERROR_WINHTTP_TIMEOUT)              msg = L"timed out — check your internet connection";
        else if (err == ERROR_WINHTTP_NAME_NOT_RESOLVED) msg = L"DNS lookup failed — check connection";
        else if (err == ERROR_WINHTTP_CANNOT_CONNECT)  msg = L"could not connect — server may be down";
        else if (err == ERROR_WINHTTP_SECURE_FAILURE)  msg = L"TLS handshake failed — system clock?";
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        onChunk(L"Error: " + msg, true);
        return;
    }

    // Check HTTP status for error surfacing
    DWORD statusCode = 0;
    DWORD scSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &scSize, WINHTTP_NO_HEADER_INDEX);

    std::string buffer;
    bool aborted = false;
    bool ioError = false;
    DWORD dwSize = 0;
    DWORD dwDownloaded = 0;
    do {
        dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
            // Distinguish "stream ended" (no more data) from "actual error/timeout"
            DWORD e = GetLastError();
            if (e != 0 && e != ERROR_WINHTTP_TIMEOUT) ioError = true;
            // Either way, no more data is coming
            break;
        }
        if (dwSize == 0) break;  // clean end of stream

        std::vector<char> chunk(dwSize + 1, 0);
        if (!WinHttpReadData(hRequest, chunk.data(), dwSize, &dwDownloaded)) {
            ioError = true;
            break;
        }

        buffer.append(chunk.data(), dwDownloaded);
        DrainSSEBuffer(buffer, onChunk, aborted);
    } while (dwSize > 0 && !aborted);

    if (!aborted) {
        // Flush any leftover (rare — usually trailing whitespace only)
        DrainSSEBuffer(buffer, onChunk, aborted);

        // HTTP error? Surface a friendly message
        if (statusCode != 0 && (statusCode < 200 || statusCode >= 300) && !aborted) {
            std::wstring statusMsg;
            switch (statusCode) {
                case 400: statusMsg = L"Bad request — check model name / payload."; break;
                case 401: statusMsg = L"401 Unauthorized — check your API key."; break;
                case 403: statusMsg = L"403 Forbidden — key valid but no access."; break;
                case 404: statusMsg = L"404 Not Found — model name probably wrong."; break;
                case 429: statusMsg = L"429 Rate limited — slow down or upgrade plan."; break;
                case 500: statusMsg = L"500 Server error — provider issue, retry shortly."; break;
                case 503: statusMsg = L"503 Service unavailable — provider overloaded."; break;
                default: {
                    wchar_t b[64];
                    wsprintfW(b, L"HTTP %lu — request failed.", statusCode);
                    statusMsg = b;
                }
            }
            onChunk(L"API Error: " + statusMsg, true);
        } else if (ioError) {
            onChunk(L"\n\n[stream cut off — network error]", true);
        } else {
            onChunk(L"", true);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
}

// -----------------------------------------------------------------------------
// OpenAI-compatible + Anthropic streaming
// -----------------------------------------------------------------------------

namespace {

struct ParsedUrl {
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring path;     // base path, no trailing slash, no scheme/host
    bool         secure = true;
};

static ParsedUrl ParseUrl(const std::string& url) {
    ParsedUrl out;
    std::string s = url;
    if (s.rfind("https://", 0) == 0) { s = s.substr(8); out.secure = true;  out.port = INTERNET_DEFAULT_HTTPS_PORT; }
    else if (s.rfind("http://", 0) == 0) { s = s.substr(7); out.secure = false; out.port = INTERNET_DEFAULT_HTTP_PORT; }
    size_t slash = s.find('/');
    std::string hostPart = (slash == std::string::npos) ? s : s.substr(0, slash);
    std::string pathPart = (slash == std::string::npos) ? "" : s.substr(slash);

    size_t colon = hostPart.find(':');
    if (colon != std::string::npos) {
        out.port = (INTERNET_PORT)std::atoi(hostPart.substr(colon + 1).c_str());
        hostPart = hostPart.substr(0, colon);
    }

    // Strip trailing slash from base path
    while (!pathPart.empty() && pathPart.back() == '/') pathPart.pop_back();

    auto utf8ToW = [](const std::string& v) {
        if (v.empty()) return std::wstring();
        int sz = MultiByteToWideChar(CP_UTF8, 0, v.data(), (int)v.size(), NULL, 0);
        std::wstring w(sz, 0);
        MultiByteToWideChar(CP_UTF8, 0, v.data(), (int)v.size(), &w[0], sz);
        return w;
    };
    out.host = utf8ToW(hostPart);
    out.path = utf8ToW(pathPart);
    return out;
}

// Forward declarations so StreamSsePost can use these helpers regardless of file order
static std::wstring ExtractJsonStringField(const std::wstring& json, const std::wstring& field);

static std::string DefaultBaseUrlFor(const std::string& provider, const std::string& configured) {
    if (provider == "openai")     return "https://api.openai.com/v1";
    if (provider == "groq")       return "https://api.groq.com/openai/v1";
    if (provider == "deepseek")   return "https://api.deepseek.com/v1";
    if (provider == "openrouter") return "https://openrouter.ai/api/v1";
    if (provider == "custom") {
        if (configured.empty()) return "https://api.openai.com/v1";
        // Allow scheme-less custom URLs
        if (configured.rfind("http://", 0) != 0 && configured.rfind("https://", 0) != 0) {
            return "https://" + configured;
        }
        return configured;
    }
    return "https://api.openai.com/v1";
}

// Generic streaming POST that drains an SSE response and hands each complete
// "data: ..." event (without the prefix, trailing newlines trimmed) to onEvent.
// onEvent returns false to stop streaming early.
// outIncomplete is set to true if the connection broke mid-stream so callers
// can warn the user the response is truncated.
// Returns true on success, false on transport failure (error already reported via onError).
static bool StreamSsePost(
    const ParsedUrl& url,
    const std::string& body,
    const std::wstring& headers,
    std::function<bool(const std::string&)> onEvent,
    std::function<void(const std::wstring&)> onError,
    bool& outIncomplete)
{
    HINTERNET hSession = WinHttpOpen(L"AIOverlay/2.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { onError(L"Error: WinHttpOpen failed"); return false; }
    WinHttpSetTimeouts(hSession, 5000, 10000, 30000, 60000);

    HINTERNET hConnect = WinHttpConnect(hSession, url.host.c_str(), url.port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); onError(L"Error: WinHttpConnect failed"); return false; }

    DWORD reqFlags = url.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", url.path.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        onError(L"Error: WinHttpOpenRequest failed");
        return false;
    }

    BOOL ok = WinHttpSendRequest(hRequest,
        headers.c_str(), (DWORD)headers.length(),
        (LPVOID)body.c_str(), (DWORD)body.length(),
        (DWORD)body.length(), 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, NULL);

    if (!ok) {
        DWORD err = GetLastError();
        std::wstring msg = L"network request failed";
        if (err == ERROR_WINHTTP_TIMEOUT)              msg = L"timed out — check your internet connection";
        else if (err == ERROR_WINHTTP_NAME_NOT_RESOLVED) msg = L"DNS lookup failed — check connection";
        else if (err == ERROR_WINHTTP_CANNOT_CONNECT)  msg = L"could not connect — server may be down";
        else if (err == ERROR_WINHTTP_SECURE_FAILURE)  msg = L"TLS handshake failed — system clock?";
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        onError(L"Error: " + msg);
        return false;
    }

    // Check HTTP status — non-2xx should be surfaced clearly
    DWORD statusCode = 0;
    DWORD scSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &scSize, WINHTTP_NO_HEADER_INDEX);
    // 200..299 ok; otherwise we still read the body to extract the error message
    bool isError = (statusCode != 0 && (statusCode < 200 || statusCode >= 300));

    std::string buffer;
    bool stop = false;
    bool ioError = false;
    DWORD dwSize = 0, dwDownloaded = 0;
    do {
        dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) {
            DWORD e = GetLastError();
            if (e != 0 && e != ERROR_WINHTTP_TIMEOUT) ioError = true;
            break;
        }
        if (dwSize == 0) break;
        std::vector<char> chunk(dwSize + 1, 0);
        if (!WinHttpReadData(hRequest, chunk.data(), dwSize, &dwDownloaded)) { ioError = true; break; }
        buffer.append(chunk.data(), dwDownloaded);

        while (!stop) {
            size_t nn = buffer.find("\n\n");
            size_t rnrn = buffer.find("\r\n\r\n");
            size_t end; size_t sepLen;
            if (nn == std::string::npos && rnrn == std::string::npos) break;
            if (nn == std::string::npos)        { end = rnrn; sepLen = 4; }
            else if (rnrn == std::string::npos) { end = nn;   sepLen = 2; }
            else if (rnrn < nn)                 { end = rnrn; sepLen = 4; }
            else                                { end = nn;   sepLen = 2; }

            std::string event = buffer.substr(0, end);
            buffer.erase(0, end + sepLen);

            size_t dataPos = event.find("data:");
            if (dataPos == std::string::npos) continue;
            size_t i = dataPos + 5;
            if (i < event.size() && event[i] == ' ') i++;
            std::string payload = event.substr(i);
            while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r')) payload.pop_back();
            if (payload.empty()) continue;
            if (payload == "[DONE]") { stop = true; break; }

            if (!onEvent(payload)) { stop = true; break; }
        }
    } while (dwSize > 0 && !stop);

    // If the HTTP response was an error and the SSE drain didn't surface it,
    // try to extract a useful message from the accumulated buffer.
    if (isError && !stop) {
        int sz = MultiByteToWideChar(CP_UTF8, 0, buffer.data(), (int)buffer.size(), NULL, 0);
        std::wstring wbody(sz, 0);
        MultiByteToWideChar(CP_UTF8, 0, buffer.data(), (int)buffer.size(), &wbody[0], sz);
        std::wstring apiMsg = ExtractJsonStringField(wbody, L"message");

        std::wstring statusMsg;
        switch (statusCode) {
            case 400: statusMsg = L"Bad request — check model name and request body."; break;
            case 401: statusMsg = L"401 Unauthorized — check your API key."; break;
            case 403: statusMsg = L"403 Forbidden — key valid but lacks permission."; break;
            case 404: statusMsg = L"404 Not Found — model name probably wrong."; break;
            case 429: statusMsg = L"429 Rate limited — slow down or upgrade plan."; break;
            case 500: statusMsg = L"500 Server error — provider issue, retry shortly."; break;
            case 503: statusMsg = L"503 Service unavailable — provider overloaded."; break;
            default: {
                wchar_t buf[64];
                wsprintfW(buf, L"HTTP %lu — request failed.", statusCode);
                statusMsg = buf;
            }
        }
        if (!apiMsg.empty()) statusMsg += L" Details: " + apiMsg;
        onError(L"API Error: " + statusMsg);
        stop = true;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    outIncomplete = ioError && !stop;
    return true;
}

// Find a JSON string field value by name. Walks once, returns first match.
// Returns empty wstring if not found.
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

}  // namespace

void LLMClient::CallOpenAICompatStreaming(
    const std::wstring& userMessage,
    const std::vector<LLMTurn>& history,
    const LLMConfig& config,
    const std::string& pngBase64,
    LLMStreamCallback onChunk)
{
    // Build body
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

    // Final user turn — array form so we can attach an image
    body << ",{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\""
         << JsonEscapeUtf8(WideToUtf8(userMessage)) << "\"}";
    if (!pngBase64.empty()) {
        body << ",{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,"
             << pngBase64 << "\"}}";
    }
    body << "]}]}";

    std::string baseUrl = DefaultBaseUrlFor(config.provider, config.base_url);
    ParsedUrl parsed = ParseUrl(baseUrl);
    parsed.path += L"/chat/completions";

    std::wstring headers = L"Content-Type: application/json\r\nAuthorization: Bearer "
                         + Utf8ToWide(config.api_key);

    bool incomplete = false;
    StreamSsePost(parsed, body.str(), headers,
        [&](const std::string& payload) -> bool {
            int sz = MultiByteToWideChar(CP_UTF8, 0, payload.data(), (int)payload.size(), NULL, 0);
            std::wstring wjson(sz, 0);
            MultiByteToWideChar(CP_UTF8, 0, payload.data(), (int)payload.size(), &wjson[0], sz);

            if (wjson.find(L"\"error\"") != std::wstring::npos) {
                std::wstring msg = ExtractJsonStringField(wjson, L"message");
                if (msg.empty()) msg = L"unknown error";
                onChunk(L"API Error: " + msg, true);
                return false;
            }
            std::wstring text = ExtractJsonStringField(wjson, L"content");
            if (!text.empty()) onChunk(text, false);
            return true;
        },
        [&](const std::wstring& err) { onChunk(err, true); },
        incomplete);

    if (incomplete) onChunk(L"\n\n[stream cut off — network error]", true);
    else            onChunk(L"", true);
}

void LLMClient::CallAnthropicStreaming(
    const std::wstring& userMessage,
    const std::vector<LLMTurn>& history,
    const LLMConfig& config,
    const std::string& pngBase64,
    LLMStreamCallback onChunk)
{
    // Body — Anthropic separates system out and uses different image schema
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

    ParsedUrl parsed;
    parsed.host = L"api.anthropic.com";
    parsed.port = INTERNET_DEFAULT_HTTPS_PORT;
    parsed.path = L"/v1/messages";
    parsed.secure = true;

    std::wstring headers = L"Content-Type: application/json\r\n"
                           L"x-api-key: " + Utf8ToWide(config.api_key) + L"\r\n"
                           L"anthropic-version: 2023-06-01";

    bool incomplete = false;
    StreamSsePost(parsed, body.str(), headers,
        [&](const std::string& payload) -> bool {
            int sz = MultiByteToWideChar(CP_UTF8, 0, payload.data(), (int)payload.size(), NULL, 0);
            std::wstring wjson(sz, 0);
            MultiByteToWideChar(CP_UTF8, 0, payload.data(), (int)payload.size(), &wjson[0], sz);

            if (wjson.find(L"\"type\":\"error\"") != std::wstring::npos
                || wjson.find(L"\"error\"") != std::wstring::npos)
            {
                std::wstring msg = ExtractJsonStringField(wjson, L"message");
                if (!msg.empty()) {
                    onChunk(L"API Error: " + msg, true);
                    return false;
                }
            }
            if (wjson.find(L"content_block_delta") != std::wstring::npos) {
                std::wstring text = ExtractJsonStringField(wjson, L"text");
                if (!text.empty()) onChunk(text, false);
            }
            return true;
        },
        [&](const std::wstring& err) { onChunk(err, true); },
        incomplete);

    if (incomplete) onChunk(L"\n\n[stream cut off — network error]", true);
    else            onChunk(L"", true);
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

// State machine that splits a merged Gemini text stream into the TRANSCRIPT line
// and the ANSWER stream. Buffers internally; emits to callbacks when complete.
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
                if (acc.size() < 5) return;  // need NONE\X or first chars of real answer
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
        // Always tell the caller the stream is over so they can finalize the bubble.
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

    // Build request body (one-shot — no history, audio only)
    std::vector<LLMTurn> empty;
    std::string body = BuildGeminiRequestBody(
        CLASSIFY_AND_ANSWER_PROMPT,
        L"Process the attached audio per your instructions.",
        empty, std::string(), wavBase64);

    std::wstring path = L"/v1beta/models/" + Utf8ToWide(config.model)
                      + L":streamGenerateContent?alt=sse";
    std::wstring headers = L"Content-Type: application/json\r\nx-goog-api-key: "
                         + Utf8ToWide(config.api_key);

    // Reuse the same WinHTTP streaming pattern as CallGeminiStreaming.
    HINTERNET hSession = WinHttpOpen(L"AIOverlay/2.4.2 Merged",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { parser.Finalize(); return; }
    WinHttpSetTimeouts(hSession, 5000, 10000, 30000, 60000);

    HINTERNET hConnect = WinHttpConnect(hSession,
        L"generativelanguage.googleapis.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); parser.Finalize(); return; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); parser.Finalize(); return; }

    BOOL ok = WinHttpSendRequest(hRequest,
        headers.c_str(), (DWORD)headers.length(),
        (LPVOID)body.c_str(), (DWORD)body.length(),
        (DWORD)body.length(), 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, NULL);
    if (!ok) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        parser.Finalize();
        return;
    }

    // Wrap the existing DrainSSEBuffer pattern. Each text chunk it extracts is
    // fed to the parser instead of straight to the user's onChunk.
    LLMStreamCallback chunkSink = [&parser](const std::wstring& text, bool /*isFinal*/) {
        if (!text.empty()) parser.Feed(text);
    };

    std::string buffer;
    bool aborted = false;
    DWORD dwSize = 0, dwDownloaded = 0;
    do {
        dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
        if (dwSize == 0) break;
        std::vector<char> chunk(dwSize + 1, 0);
        if (!WinHttpReadData(hRequest, chunk.data(), dwSize, &dwDownloaded)) break;
        buffer.append(chunk.data(), dwDownloaded);
        DrainSSEBuffer(buffer, chunkSink, aborted);
    } while (dwSize > 0 && !aborted);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    parser.Finalize();
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
