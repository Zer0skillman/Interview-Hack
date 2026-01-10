#pragma once
#include <string>
#include "ConfigLoader.h"

class LLMClient {
public:
    static std::wstring GenerateContent(const std::wstring& prompt, const LLMConfig& config);

private:
    static std::wstring CallGemini(const std::wstring& prompt, const LLMConfig& config);
    
    // Helpers
    static std::string WideToUtf8(const std::wstring& wstr);
    static std::wstring Utf8ToWide(const std::string& str);
    static std::wstring ExtractTextFromJson(const std::wstring& json);
    static std::wstring SendRequest(const std::wstring& host, const std::wstring& path, const std::string& jsonBody, const std::wstring& headers, int port = 8000);
};
