#include "LLMClient.h"
#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <sstream>

#pragma comment(lib, "winhttp.lib")

std::wstring LLMClient::GenerateContent(const std::wstring& prompt, const LLMConfig& config) {
    // We now act as a dummy client forwarding to our local Python backend
    // config.api_key and model are used by the backend, not here (passed via env vars)
    
    std::wstring host = L"127.0.0.1";
    std::wstring path = L"/chat";
    int port = 8000; // Use config port if needed, hardcoded for now matching backend default

    std::string promptUtf8 = WideToUtf8(prompt);
    
    // JSON Escape
    std::string jsonBody = "{ \"text\": \"";
    for (char c : promptUtf8) {
        if (c == '\"') jsonBody += "\\\"";
        else if (c == '\\') jsonBody += "\\\\";
        else if (c == '\n') jsonBody += "\\n";
        else if (c == '\r') jsonBody += "\\r";
        else jsonBody += c;
    }
    jsonBody += "\" }";

    std::wstring headers = L"Content-Type: application/json";
    
    std::wstring responseJson = SendRequest(host, path, jsonBody, headers, port);
    return ExtractTextFromJson(responseJson);
}

std::wstring LLMClient::SendRequest(const std::wstring& host, const std::wstring& path, const std::string& jsonBody, const std::wstring& headers, int port) {
    HINTERNET hSession = WinHttpOpen(L"AIOverlay/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return L"Error: WinHttpOpen failed";

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return L"Error: WinHttpConnect failed (Is backend running?)";
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0); // Removed Secure flag for localhost
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
        responseData = "Error: Backend request failed";
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return Utf8ToWide(responseData);
}

std::wstring LLMClient::ExtractTextFromJson(const std::wstring& json) {
    // Simple parser for extracting "reply" field
    // response format: { "reply": "AI response" }
    
    std::wstring key = L"\"reply\": \"";
    size_t pos = json.find(key);
    if (pos == std::wstring::npos) {
        // Fallback or error check
        return json; // Return raw if parsing fails (might be plain error string)
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
            // Simple unicode support same as before
            else if (c == 'u' && i + 4 < json.length()) {
                 try {
                     result += static_cast<wchar_t>(std::stoi(json.substr(i+1, 4), nullptr, 16));
                     i+=4;
                 } catch(...) { result += L"\\u"; }
            }
            else result += c;
            escape = false;
        } else {
            if (c == L'\\') escape = true;
            else if (c == L'\"') break;
            else result += c;
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
