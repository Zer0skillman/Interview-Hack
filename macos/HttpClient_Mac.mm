#ifdef __APPLE__

#include "../HttpClient.h"

#include <curl/curl.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>

// macOS ships libcurl in /usr/lib. CMake picks it up via find_package(CURL).
// We deliberately keep this file libcurl-only — no Objective-C bridging needed
// since the LLM HTTP surface is small (POST, GET, SSE, file download).

namespace {

static std::once_flag s_curlInit;
static void EnsureCurlInit() {
    std::call_once(s_curlInit, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

struct WriteCtx {
    std::string* dest;
};

static size_t WriteToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<WriteCtx*>(userdata);
    size_t bytes = size * nmemb;
    ctx->dest->append(ptr, bytes);
    return bytes;
}

struct StreamCtx {
    std::string buffer;
    std::function<bool(const std::string&)> onEvent;
    bool stop = false;
    bool isError = false;
    std::string errorBody;
};

// Splits the libcurl receive buffer into complete SSE events on "\n\n" or
// "\r\n\r\n" boundaries. For each event, extracts the "data:" payload and
// hands it to onEvent. Stops if onEvent returns false or payload is [DONE].
static void DrainSse(StreamCtx& ctx) {
    while (!ctx.stop) {
        size_t nn = ctx.buffer.find("\n\n");
        size_t rnrn = ctx.buffer.find("\r\n\r\n");
        size_t end, sepLen;
        if (nn == std::string::npos && rnrn == std::string::npos) break;
        if (nn == std::string::npos)        { end = rnrn; sepLen = 4; }
        else if (rnrn == std::string::npos) { end = nn;   sepLen = 2; }
        else if (rnrn < nn)                 { end = rnrn; sepLen = 4; }
        else                                { end = nn;   sepLen = 2; }

        std::string event = ctx.buffer.substr(0, end);
        ctx.buffer.erase(0, end + sepLen);

        size_t dataPos = event.find("data:");
        if (dataPos == std::string::npos) continue;
        size_t i = dataPos + 5;
        if (i < event.size() && event[i] == ' ') i++;
        std::string payload = event.substr(i);
        while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
            payload.pop_back();
        if (payload.empty()) continue;
        if (payload == "[DONE]") { ctx.stop = true; break; }

        if (!ctx.onEvent(payload)) { ctx.stop = true; break; }
    }
}

static size_t WriteStream(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    size_t bytes = size * nmemb;
    if (ctx->isError) {
        ctx->errorBody.append(ptr, bytes);
        return bytes;
    }
    ctx->buffer.append(ptr, bytes);
    DrainSse(*ctx);
    return bytes;
}

struct FileCtx {
    std::ofstream* file;
    std::function<void(int)> onPercent;
    int lastPct = -1;
    double total = 0;
};

static size_t WriteToFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<FileCtx*>(userdata);
    size_t bytes = size * nmemb;
    ctx->file->write(ptr, (std::streamsize)bytes);
    return bytes;
}

static int XferInfo(void* userdata, curl_off_t dltotal, curl_off_t dlnow,
                    curl_off_t, curl_off_t) {
    auto* ctx = static_cast<FileCtx*>(userdata);
    if (dltotal > 0 && ctx->onPercent) {
        int pct = (int)((100LL * dlnow) / dltotal);
        if (pct != ctx->lastPct) {
            ctx->onPercent(pct);
            ctx->lastPct = pct;
        }
    }
    return 0;
}

static struct curl_slist* BuildHeaderList(const std::vector<std::string>& headers) {
    struct curl_slist* list = nullptr;
    for (const auto& h : headers) {
        list = curl_slist_append(list, h.c_str());
    }
    return list;
}

static std::string FriendlyError(CURLcode code) {
    switch (code) {
        case CURLE_OPERATION_TIMEDOUT:    return "timed out — check your internet connection";
        case CURLE_COULDNT_RESOLVE_HOST:  return "DNS lookup failed — check connection";
        case CURLE_COULDNT_CONNECT:       return "could not connect — server may be down";
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
                                          return "TLS handshake failed — system clock?";
        default: {
            const char* msg = curl_easy_strerror(code);
            return std::string("network request failed (") + (msg ? msg : "unknown") + ")";
        }
    }
}

}  // namespace

namespace HttpClient {

HttpResponse Post(const std::string& url, const std::string& body,
                  const std::vector<std::string>& headers)
{
    EnsureCurlInit();
    HttpResponse r;

    CURL* curl = curl_easy_init();
    if (!curl) { r.errorMsg = "Error: curl_easy_init failed"; return r; }

    struct curl_slist* hdr = BuildHeaderList(headers);

    WriteCtx ctx{&r.body};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    if (hdr) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 90L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AIOverlay/2.0");

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        long sc = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &sc);
        r.statusCode = (int)sc;
    } else {
        r.errorMsg = "Error: " + FriendlyError(rc);
    }

    if (hdr) curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);
    return r;
}

HttpResponse Get(const std::string& url, const std::vector<std::string>& headers)
{
    EnsureCurlInit();
    HttpResponse r;

    CURL* curl = curl_easy_init();
    if (!curl) { r.errorMsg = "Error: curl_easy_init failed"; return r; }

    struct curl_slist* hdr = BuildHeaderList(headers);

    WriteCtx ctx{&r.body};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    if (hdr) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AIOverlay-Updater");

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        long sc = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &sc);
        r.statusCode = (int)sc;
    } else {
        r.errorMsg = "Error: " + FriendlyError(rc);
    }

    if (hdr) curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);
    return r;
}

HttpResponse StreamPost(const std::string& url, const std::string& body,
                        const std::vector<std::string>& headers,
                        std::function<bool(const std::string&)> onEvent)
{
    EnsureCurlInit();
    HttpResponse r;

    CURL* curl = curl_easy_init();
    if (!curl) { r.errorMsg = "Error: curl_easy_init failed"; return r; }

    struct curl_slist* hdr = BuildHeaderList(headers);

    StreamCtx ctx;
    ctx.onEvent = onEvent;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    if (hdr) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteStream);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);            // no overall timeout for streaming
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AIOverlay/2.0");

    // Peek at the response code as soon as headers arrive so we can decide
    // whether to dispatch chunks to onEvent or accumulate as an error body.
    // We do that inside WriteStream by checking ctx.isError; toggle it here
    // after curl_easy_perform finishes by inspecting the response.
    CURLcode rc = curl_easy_perform(curl);
    long sc = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &sc);
    r.statusCode = (int)sc;

    if (rc != CURLE_OK) {
        r.errorMsg = "Error: " + FriendlyError(rc);
    }

    // Surface accumulated buffer if the response was an error and we hadn't
    // toggled isError mid-stream (i.e. WriteStream already routed events).
    if (sc != 0 && (sc < 200 || sc >= 300)) {
        r.body = !ctx.errorBody.empty() ? ctx.errorBody : ctx.buffer;
    }

    if (hdr) curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);
    return r;
}

bool Download(const std::string& url, const std::string& destPath,
              std::function<void(int)> onPercent)
{
    EnsureCurlInit();

    std::ofstream out(destPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    FileCtx ctx{&out, std::move(onPercent), -1, 0};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, XferInfo);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AIOverlay-Updater");

    CURLcode rc = curl_easy_perform(curl);
    bool ok = (rc == CURLE_OK);

    long sc = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &sc);
    if (ok && (sc < 200 || sc >= 300)) ok = false;

    curl_easy_cleanup(curl);
    out.close();

    if (ok && ctx.onPercent && ctx.lastPct != 100) ctx.onPercent(100);
    return ok;
}

}  // namespace HttpClient

#endif // __APPLE__
