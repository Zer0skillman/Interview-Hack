#pragma once
#include <string>
#include "ConfigLoader.h"

class LLMClient {
public:
    static std::wstring GenerateContent(const std::wstring& prompt, const LLMConfig& config);

private:
    // Main dispatchers
    static std::wstring CallGemini(const std::wstring& prompt, const LLMConfig& config);
    static std::wstring CallOpenAI(const std::wstring& prompt, const LLMConfig& config); // Future support
    static std::wstring CallGrok(const std::wstring& prompt, const LLMConfig& config);   // Future support

    // Helpers
    static std::string WideToUtf8(const std::wstring& wstr);
    static std::wstring Utf8ToWide(const std::string& str);
    static std::wstring ExtractTextFromJson(const std::wstring& json, const std::string& provider);
    static std::wstring SendRequest(const std::wstring& host, const std::wstring& path, const std::string& jsonBody, const std::wstring& headers);
};
