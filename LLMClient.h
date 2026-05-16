#pragma once
#include <string>
#include <vector>
#include <functional>
#include "ConfigLoader.h"

struct LLMTurn {
    bool isUser;
    std::wstring text;
};

// onChunk(text, isFinal): called for each chunk; isFinal==true on the last call
// with an empty text means the stream ended cleanly. If a chunk text starts with
// "Error:" the stream is aborted and isFinal will be true on that same call.
using LLMStreamCallback = std::function<void(const std::wstring& chunk, bool isFinal)>;

class LLMClient {
public:
    // Non-streaming. pngBase64 / wavBase64 are optional inline parts.
    static std::wstring GenerateContent(
        const std::wstring& userMessage,
        const std::vector<LLMTurn>& history,
        const LLMConfig& config,
        const std::string& pngBase64 = "",
        const std::string& wavBase64 = "");

    // Streaming variant: fires onChunk per Gemini SSE chunk. Blocking on the caller's thread.
    static void GenerateContentStreaming(
        const std::wstring& userMessage,
        const std::vector<LLMTurn>& history,
        const LLMConfig& config,
        const std::string& pngBase64,
        const std::string& wavBase64,
        LLMStreamCallback onChunk);

    // True if the provider can accept inline audio bytes (only Gemini today).
    static bool ProviderSupportsAudio(const std::string& provider);

    // UTF-8 <-> wstring helpers used across the codebase (clipboard send,
    // window persistence, hotkey labels). Exposed because callers in
    // MacOverlayWindow.mm and elsewhere need the same conversion.
    static std::string  WideToUtf8(const std::wstring& wstr);
    static std::wstring Utf8ToWide(const std::string& str);
    static std::string  JsonEscapeUtf8(const std::string& utf8);

    struct ClassifyResult {
        std::wstring transcript;
        std::wstring questionText;   // non-empty iff a substantive question was detected
    };

    // Background-poll path: send raw audio to Gemini with a strict prompt that
    // returns a TRANSCRIPT line and (optionally) a QUESTION line. Non-streaming,
    // blocking. Returned shape: parsed into ClassifyResult.
    // Kept for callers that want the 2-call architecture (classifier + separate
    // streaming answer). The single-call merged path is ClassifyAndAnswerStreaming.
    static ClassifyResult ClassifyAndTranscribe(
        const std::string& wavBase64,
        const LLMConfig& config);

    // Single-call merged path: streams TRANSCRIPT + (optional) ANSWER from one
    // Gemini request. Saves an extra round-trip vs the 2-call architecture.
    //
    // onTranscript fires once when the TRANSCRIPT line completes.
    // onAnswerChunk fires per chunk of the ANSWER (or never, if response is NONE).
    // The terminal `isFinal=true` call on onAnswerChunk always fires.
    static void ClassifyAndAnswerStreaming(
        const std::string& wavBase64,
        const LLMConfig& config,
        std::function<void(const std::wstring&)> onTranscript,
        LLMStreamCallback onAnswerChunk);

private:
    static std::wstring CallGemini(
        const std::wstring& userMessage,
        const std::vector<LLMTurn>& history,
        const LLMConfig& config,
        const std::string& pngBase64,
        const std::string& wavBase64);

    static void CallGeminiStreaming(
        const std::wstring& userMessage,
        const std::vector<LLMTurn>& history,
        const LLMConfig& config,
        const std::string& pngBase64,
        const std::string& wavBase64,
        LLMStreamCallback onChunk);

    static std::string BuildGeminiRequestBody(
        const std::wstring& systemPrompt,
        const std::wstring& userMessage,
        const std::vector<LLMTurn>& history,
        const std::string& pngBase64,
        const std::string& wavBase64);

    // OpenAI-compatible: works for OpenAI, Groq, DeepSeek, OpenRouter, Ollama, custom.
    static void CallOpenAICompatStreaming(
        const std::wstring& userMessage,
        const std::vector<LLMTurn>& history,
        const LLMConfig& config,
        const std::string& pngBase64,
        LLMStreamCallback onChunk);

    static void CallAnthropicStreaming(
        const std::wstring& userMessage,
        const std::vector<LLMTurn>& history,
        const LLMConfig& config,
        const std::string& pngBase64,
        LLMStreamCallback onChunk);

    static std::wstring SendHttpsRequest(
        const std::wstring& host,
        const std::wstring& path,
        const std::string& jsonBody,
        const std::wstring& headers);

    static std::wstring ExtractFirstTextField(const std::wstring& json);
    static std::wstring ExtractErrorMessage(const std::wstring& json);
};
