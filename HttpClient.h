#pragma once
#include <string>
#include <vector>
#include <functional>

// Cross-platform HTTP client. Two impls behind the same interface:
//
//   - HttpClient_Win.cpp — WinHTTP, used on Windows
//   - HttpClient_Mac.mm  — libcurl (ships with macOS), used on Apple
//
// LLMClient and Updater call into this so they don't carry platform HTTP code
// directly. The contract is intentionally minimal — just what those two
// callers need.

struct HttpResponse {
    int statusCode = 0;           // 0 means transport error (see errorMsg)
    std::string body;             // full body for Post/Get; empty for StreamPost
    std::string errorMsg;         // empty on success; friendly message otherwise
};

namespace HttpClient {

// Synchronous POST. Returns full response body in resp.body. headers is a
// list of "Name: Value" strings.
HttpResponse Post(const std::string& url,
                  const std::string& body,
                  const std::vector<std::string>& headers);

// Synchronous GET.
HttpResponse Get(const std::string& url,
                 const std::vector<std::string>& headers);

// Streaming POST. onEvent receives each complete "data: ..." event payload
// (just the part AFTER "data:", with leading space + trailing CRLF trimmed).
// Return false from onEvent to stop early.
// HttpResponse.statusCode is the final HTTP status. HttpResponse.body is empty
// on success but contains the response body if statusCode is non-2xx (so the
// caller can extract an error message).
HttpResponse StreamPost(const std::string& url,
                        const std::string& body,
                        const std::vector<std::string>& headers,
                        std::function<bool(const std::string&)> onEvent);

// Downloads URL to destPath, calling onPercent(0..100) on each progress tick.
// Returns true on success. Used by the Updater for the release zip download.
bool Download(const std::string& url,
              const std::string& destPath,
              std::function<void(int)> onPercent);

} // namespace HttpClient
