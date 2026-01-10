#pragma once
#include <string>
#include <map>
#include <vector>

struct LLMConfig {
    std::string provider; // "gemini", "openai", "grok"
    std::string model;
    std::string api_key;
};

struct ModelInfo {
    std::string id;
    std::string name;
};

class ConfigLoader {
public:
    static LLMConfig LoadConfig(const std::string& filepath);
    static void SaveConfig(const std::string& filepath, const LLMConfig& config);
    static std::vector<ModelInfo> LoadModels(const std::string& filepath);
};
