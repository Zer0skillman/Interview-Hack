#include "ConfigLoader.h"
#include <fstream>
#include <sstream>

LLMConfig ConfigLoader::LoadConfig(const std::string& filepath) {
    LLMConfig config;
    std::ifstream file(filepath);
    
    // Defaults
    config.provider = "gemini";
    config.model = "gemini-2.5-flash-lite";
    
    if (!file.is_open()) return config;

    std::string line;
    while (std::getline(file, line)) {
        size_t delimiterPos = line.find('=');
        if (delimiterPos != std::string::npos) {
            std::string key = line.substr(0, delimiterPos);
            std::string value = line.substr(delimiterPos + 1);

            // Simple trim logic can be added if needed, but assuming clean file for now
            if (key == "model") config.model = value;
            else if (key == "api_key") config.api_key = value;
            else if (key == "provider") config.provider = value;
        }
    }
    return config;
}

void ConfigLoader::SaveConfig(const std::string& filepath, const LLMConfig& config) {
    std::ofstream file(filepath);
    if (file.is_open()) {
        file << "provider=" << config.provider << "\n";
        file << "model=" << config.model << "\n";
        file << "api_key=" << config.api_key << "\n";
    }
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
