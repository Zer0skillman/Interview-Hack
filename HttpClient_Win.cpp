#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include "HttpClient.h"

#include <windows.h>
#include <winhttp.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace {

struct ParsedUrl {
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring path;
    bool         secure = true;
};

static std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string WToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

static ParsedUrl ParseUrl(const std::string& url) {
    ParsedUrl out;
    std::string s = url;
    if (s.rfind("https://", 0) == 0) { s = s.substr(8); out.secure = true;  out.port = INTERNET_DEFAULT_HTTPS_PORT; }
    else if (s.rfind("http://", 0) == 0) { s = s.substr(7); out.secure = false; out.port = INTERNET_DEFAULT_HTTP_PORT; }
    size_t slash = s.find('/');
    std::string hostPart = (slash == std::string::npos) ? s : s.substr(0, slash);
    std::string pathPart = (slash == std::string::npos) ? "/" : s.substr(slash);

    size_t colon = hostPart.find(':');
    if (colon != std::string::npos) {
        out.port = (INTERNET_PORT)std::atoi(hostPart.substr(colon + 1).c_str());
        hostPart = hostPart.substr(0, colon);
    }

    out.host = Utf8ToW(hostPart);
    out.path = Utf8ToW(pathPart);
    return out;
}

static std::wstring JoinHeaders(const std::vector<std::string>& headers) {
    std::wstring out;
    for (size_t i = 0; i < headers.size(); ++i) {
        if (i) out += L"\r\n";
        out += Utf8ToW(headers[i]);
    }
    return out;
}

static std::string FriendlyError(DWORD err) {
    switch (err) {
        case ERROR_WINHTTP_TIMEOUT:           return "timed out — check your internet connection";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "DNS lookup failed — check connection";
        case ERROR_WINHTTP_CANNOT_CONNECT:    return "could not connect — server may be down";
        case ERROR_WINHTTP_SECURE_FAILURE:    return "TLS handshake failed — system clock?";
        default: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "network request failed (code %lu)", err);
            return std::string(buf);
        }
    }
}

struct Session {
    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hRequest = nullptr;

    ~Session() {
        if (hRequest) WinHttpCloseHandle(hRequest);
        if (hConnect) WinHttpCloseHandle(hConnect);
        if (hSession) WinHttpCloseHandle(hSession);
    }

    bool Open(const ParsedUrl& url, const wchar_t* method, const std::wstring& headers,
              HttpResponse& outErr,
              int resolveMs, int connectMs, int sendMs, int receiveMs)
    {
        hSession = WinHttpOpen(L"AIOverlay/2.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) { outErr.errorMsg = "Error: WinHttpOpen failed"; return false; }
        WinHttpSetTimeouts(hSession, resolveMs, connectMs, sendMs, receiveMs);

        hConnect = WinHttpConnect(hSession, url.host.c_str(), url.port, 0);
        if (!hConnect) { outErr.errorMsg = "Error: WinHttpConnect failed"; return false; }

        DWORD flags = url.secure ? WINHTTP_FLAG_SECURE : 0;
        hRequest = WinHttpOpenRequest(hConnect, method, url.path.c_str(),
            NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hRequest) { outErr.errorMsg = "Error: WinHttpOpenRequest failed"; return false; }

        // Follow redirects (Updater needs this for the GitHub asset URL).
        DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

        BOOL ok;
        if (method == std::wstring(L"GET")) {
            ok = WinHttpSendRequest(hRequest,
                headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                headers.empty() ? 0 : (DWORD)headers.length(),
                NULL, 0, 0, 0);
        } else {
            ok = TRUE;  // Caller calls SendRequest for POST.
            (void)ok;
        }
        return true;
    }
};

}  // namespace

namespace HttpClient {

HttpResponse Post(const std::string& url, const std::string& body,
                  const std::vector<std::string>& headers)
{
    HttpResponse r;
    ParsedUrl parsed = ParseUrl(url);
    std::wstring hdr = JoinHeaders(headers);

    Session s;
    if (!s.Open(parsed, L"POST", L"", r, 5000, 10000, 30000, 60000)) return r;

    BOOL ok = WinHttpSendRequest(s.hRequest,
        hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(),
        hdr.empty() ? 0 : (DWORD)hdr.length(),
        (LPVOID)body.c_str(), (DWORD)body.length(),
        (DWORD)body.length(), 0);
    if (ok) ok = WinHttpReceiveResponse(s.hRequest, NULL);
    if (!ok) {
        r.errorMsg = "Error: " + FriendlyError(GetLastError());
        return r;
    }

    DWORD sc = 0, scSize = sizeof(sc);
    WinHttpQueryHeaders(s.hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &sc, &scSize, WINHTTP_NO_HEADER_INDEX);
    r.statusCode = (int)sc;

    DWORD avail = 0, got = 0;
    while (WinHttpQueryDataAvailable(s.hRequest, &avail) && avail > 0) {
        std::vector<char> buf(avail + 1, 0);
        if (!WinHttpReadData(s.hRequest, buf.data(), avail, &got)) break;
        r.body.append(buf.data(), got);
    }
    return r;
}

HttpResponse Get(const std::string& url, const std::vector<std::string>& headers)
{
    HttpResponse r;
    ParsedUrl parsed = ParseUrl(url);
    std::wstring hdr = JoinHeaders(headers);

    Session s;
    if (!s.Open(parsed, L"GET", hdr, r, 5000, 10000, 30000, 60000)) return r;

    if (!WinHttpReceiveResponse(s.hRequest, NULL)) {
        r.errorMsg = "Error: " + FriendlyError(GetLastError());
        return r;
    }

    DWORD sc = 0, scSize = sizeof(sc);
    WinHttpQueryHeaders(s.hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &sc, &scSize, WINHTTP_NO_HEADER_INDEX);
    r.statusCode = (int)sc;

    DWORD avail = 0, got = 0;
    while (WinHttpQueryDataAvailable(s.hRequest, &avail) && avail > 0) {
        std::vector<char> buf(avail + 1, 0);
        if (!WinHttpReadData(s.hRequest, buf.data(), avail, &got)) break;
        r.body.append(buf.data(), got);
        if (r.body.size() > 5 * 1024 * 1024) break;  // sanity cap
    }
    return r;
}

HttpResponse StreamPost(const std::string& url, const std::string& body,
                        const std::vector<std::string>& headers,
                        std::function<bool(const std::string&)> onEvent)
{
    HttpResponse r;
    ParsedUrl parsed = ParseUrl(url);
    std::wstring hdr = JoinHeaders(headers);

    Session s;
    if (!s.Open(parsed, L"POST", L"", r, 5000, 10000, 30000, 60000)) return r;

    BOOL ok = WinHttpSendRequest(s.hRequest,
        hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(),
        hdr.empty() ? 0 : (DWORD)hdr.length(),
        (LPVOID)body.c_str(), (DWORD)body.length(),
        (DWORD)body.length(), 0);
    if (ok) ok = WinHttpReceiveResponse(s.hRequest, NULL);
    if (!ok) {
        r.errorMsg = "Error: " + FriendlyError(GetLastError());
        return r;
    }

    DWORD sc = 0, scSize = sizeof(sc);
    WinHttpQueryHeaders(s.hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &sc, &scSize, WINHTTP_NO_HEADER_INDEX);
    r.statusCode = (int)sc;
    bool isError = (sc != 0 && (sc < 200 || sc >= 300));

    std::string buffer;
    bool stop = false;
    DWORD avail = 0, got = 0;
    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(s.hRequest, &avail)) break;
        if (avail == 0) break;
        std::vector<char> chunk(avail + 1, 0);
        if (!WinHttpReadData(s.hRequest, chunk.data(), avail, &got)) break;
        buffer.append(chunk.data(), got);

        if (isError) continue;  // Accumulate body for caller to surface error.

        while (!stop) {
            size_t nn = buffer.find("\n\n");
            size_t rnrn = buffer.find("\r\n\r\n");
            size_t end, sepLen;
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
    } while (avail > 0 && !stop);

    if (isError) r.body = buffer;
    return r;
}

bool Download(const std::string& url, const std::string& destPath,
              std::function<void(int)> onPercent)
{
    ParsedUrl parsed = ParseUrl(url);
    HttpResponse junk;
    Session s;
    if (!s.Open(parsed, L"GET", L"User-Agent: AIOverlay-Updater", junk,
                5000, 10000, 60000, 120000)) {
        return false;
    }

    if (!WinHttpReceiveResponse(s.hRequest, NULL)) return false;

    DWORD contentLen = 0, lenSz = sizeof(contentLen);
    WinHttpQueryHeaders(s.hRequest,
        WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &contentLen, &lenSz, WINHTTP_NO_HEADER_INDEX);

    HANDLE hFile = CreateFileA(destPath.c_str(), GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD total = 0;
    int lastPct = -1;
    bool ok = true;
    while (true) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(s.hRequest, &avail)) { ok = false; break; }
        if (avail == 0) break;
        std::vector<char> buf(avail);
        DWORD got = 0;
        if (!WinHttpReadData(s.hRequest, buf.data(), avail, &got)) { ok = false; break; }
        DWORD wrote = 0;
        WriteFile(hFile, buf.data(), got, &wrote, NULL);
        total += got;
        if (contentLen > 0 && onPercent) {
            int pct = (int)((100ULL * total) / contentLen);
            if (pct != lastPct) { onPercent(pct); lastPct = pct; }
        }
    }

    CloseHandle(hFile);
    if (ok && onPercent && lastPct != 100) onPercent(100);
    return ok;
}

}  // namespace HttpClient

#endif // _WIN32
