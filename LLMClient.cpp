#include "LLMClient.h"
#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <sstream>

#pragma comment(lib, "winhttp.lib")

std::wstring LLMClient::GenerateContent(const std::wstring& prompt, const LLMConfig& config) {
    if (config.api_key.empty() || config.api_key == "INSERT_KEY_HERE") {
        return L"Error: API Key not set in llm_config.txt";
    }

    if (config.provider == "gemini") {
        return CallGemini(prompt, config);
    } 
    else if (config.provider == "openai") {
        return L"Error: OpenAI support coming soon. (Check llm_config.txt provider)";
    }
    else if (config.provider == "grok") {
        return L"Error: Grok support coming soon. (Check llm_config.txt provider)";
    }

    return L"Error: Unknown provider '" + Utf8ToWide(config.provider) + L"'";
}

std::wstring LLMClient::CallGemini(const std::wstring& prompt, const LLMConfig& config) {
    std::wstring host = L"generativelanguage.googleapis.com";
    std::wstring path = L"/v1beta/models/" + Utf8ToWide(config.model) + L":generateContent?key=" + Utf8ToWide(config.api_key);
    
    std::string promptUtf8 = WideToUtf8(prompt);
    std::string jsonBody = "{ \"contents\": [{ \"parts\": [{ \"text\": \"";
    
    // Simple escape
    for (char c : promptUtf8) {
        if (c == '\"') jsonBody += "\\\"";
        else if (c == '\\') jsonBody += "\\\\";
        else if (c == '\n') jsonBody += "\\n";
        else if (c == '\r') jsonBody += "\\r";
        else jsonBody += c;
    }
    jsonBody += "\" }] }] }";

    std::wstring headers = L"Content-Type: application/json";
    
    std::wstring responseJson = SendRequest(host, path, jsonBody, headers);
    return ExtractTextFromJson(responseJson, "gemini");
}
// Future implementations for CallOpenAI, CallGrok would go here...

std::wstring LLMClient::SendRequest(const std::wstring& host, const std::wstring& path, const std::string& jsonBody, const std::wstring& headers) {
    HINTERNET hSession = WinHttpOpen(L"AIOverlay/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return L"Error: WinHttpOpen failed";

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return L"Error: WinHttpConnect failed";
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return L"Error: WinHttpOpenRequest failed";
    }

    BOOL bResults = WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)headers.length(),
        (LPVOID)jsonBody.c_str(), (DWORD)jsonBody.length(),
        (DWORD)jsonBody.length(), 0);

    if (bResults) {
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    }

    std::string responseData;
    if (bResults) {
        DWORD dwSize = 0;
        DWORD dwDownloaded = 0;
        do {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
            if (dwSize == 0) break;

            char* pszOutBuffer = new char[dwSize + 1];
            if (!pszOutBuffer) break;
            ZeroMemory(pszOutBuffer, dwSize + 1);

            if (WinHttpReadData(hRequest, (LPVOID)pszOutBuffer, dwSize, &dwDownloaded)) {
                responseData.append(pszOutBuffer, dwDownloaded);
            }
            delete[] pszOutBuffer;
        } while (dwSize > 0);
    }
    else {
        responseData = "Error: Network request failed";
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return Utf8ToWide(responseData);
}

std::wstring LLMClient::ExtractTextFromJson(const std::wstring& json, const std::string& provider) {
    // Simple parser for now, mainly targeted at Gemini's structure
    // { "candidates": [ { "content": { "parts": [ { "text": "THE ANSWER" } ] } } ] }
    
    std::wstring key = L"\"text\": \"";
    size_t pos = json.find(key);
    if (pos == std::wstring::npos) {
        if (json.find(L"\"error\":") != std::wstring::npos) {
            return L"API Error: Check config or key.";
        }
        return L"Error parsing response";
    }

    pos += key.length();
    
    std::wstring result;
    bool escape = false;
    for (size_t i = pos; i < json.length(); i++) {
        wchar_t c = json[i];
        if (escape) {
            if (c == 'n') result += L'\n';
            else if (c == 'r') result += L'\r';
            else if (c == 't') result += L'\t';
            else if (c == '\"') result += L'\"';
            else if (c == '\\') result += L'\\';
            else if (c == '/') result += L'/'; // Solidus
            else if (c == 'u') {
                // Unicode escape \uXXXX
                if (i + 4 < json.length()) {
                    std::wstring hexCode = json.substr(i + 1, 4);
                    try {
                        int code = std::stoi(hexCode, nullptr, 16);
                        result += static_cast<wchar_t>(code);
                        i += 4;
                    } catch (...) {
                        // Fallback if parsing fails
                        result += L"\\u";
                        result += hexCode;
                        i += 4; 
                    }
                } else {
                     result += L"\\u";
                }
            }
            else {
                result += c; 
            }
            escape = false;
        } else {
            if (c == L'\\') {
                escape = true;
            } else if (c == L'\"') {
                break; 
            } else {
                result += c;
            }
        }
    }

    return result;
}

std::string LLMClient::WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::wstring LLMClient::Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}
