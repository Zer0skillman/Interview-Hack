#define WIN32_LEAN_AND_MEAN
#include "Updater.h"

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <thread>
#include <mutex>

#include "Parsers.h"  // ExtractJsonStringField

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace Updater {

// Module state — Status::Ready stores the path to the staged new exe
static std::mutex s_mutex;
static std::wstring s_stagedNewExePath;
static std::wstring s_stagedTag;

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

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

static int ParseVersionPart(const std::wstring& v, int idx) {
    int a, b, c;
    a = b = c = 0;
    int got = swscanf(v.c_str(), L"%d.%d.%d", &a, &b, &c);
    (void)got;
    if (idx == 0) return a;
    if (idx == 1) return b;
    return c;
}

static bool IsNewer(const std::wstring& remote, const std::wstring& current) {
    int ra = ParseVersionPart(remote, 0), rb = ParseVersionPart(remote, 1), rc = ParseVersionPart(remote, 2);
    int ca = ParseVersionPart(current, 0), cb = ParseVersionPart(current, 1), cc = ParseVersionPart(current, 2);
    if (ra != ca) return ra > ca;
    if (rb != cb) return rb > cb;
    return rc > cc;
}

// ---------------------------------------------------------------------------
// HTTPS GET — text body (small, all in memory)
// ---------------------------------------------------------------------------

static bool HttpsGetText(const std::string& url, std::string& outBody) {
    if (url.rfind("https://", 0) != 0) return false;
    std::string s = url.substr(8);
    size_t slash = s.find('/');
    std::wstring host = Utf8ToW(slash == std::string::npos ? s : s.substr(0, slash));
    std::wstring path = Utf8ToW(slash == std::string::npos ? "/" : s.substr(slash));

    HINTERNET sess = WinHttpOpen(L"AIOverlay/Updater",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) return false;
    WinHttpSetTimeouts(sess, 5000, 10000, 10000, 30000);

    HINTERNET conn = WinHttpConnect(sess, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) { WinHttpCloseHandle(sess); return false; }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(sess); return false; }

    DWORD flags = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &flags, sizeof(flags));

    BOOL ok = WinHttpSendRequest(req,
        L"Accept: application/json\r\nUser-Agent: AIOverlay-Updater",
        -1L, NULL, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(req, NULL);

    if (ok) {
        DWORD avail = 0, downloaded = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
            std::vector<char> buf(avail + 1, 0);
            if (!WinHttpReadData(req, buf.data(), avail, &downloaded)) break;
            outBody.append(buf.data(), downloaded);
            if (outBody.size() > 5 * 1024 * 1024) break;  // sanity cap
        }
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(sess);
    return ok && !outBody.empty();
}

// ---------------------------------------------------------------------------
// HTTPS GET — binary body streamed to a file with progress callback
// ---------------------------------------------------------------------------

static bool HttpsDownloadToFile(const std::string& url, const std::wstring& destPath,
                                std::function<void(int)> onPercent)
{
    if (url.rfind("https://", 0) != 0) return false;
    std::string s = url.substr(8);
    size_t slash = s.find('/');
    std::wstring host = Utf8ToW(slash == std::string::npos ? s : s.substr(0, slash));
    std::wstring path = Utf8ToW(slash == std::string::npos ? "/" : s.substr(slash));

    HINTERNET sess = WinHttpOpen(L"AIOverlay/Updater",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) return false;
    WinHttpSetTimeouts(sess, 5000, 10000, 60000, 120000);

    HINTERNET conn = WinHttpConnect(sess, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) { WinHttpCloseHandle(sess); return false; }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(sess); return false; }

    DWORD flags = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &flags, sizeof(flags));

    BOOL ok = WinHttpSendRequest(req,
        L"User-Agent: AIOverlay-Updater",
        -1L, NULL, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(req, NULL);
    if (!ok) {
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(sess);
        return false;
    }

    // Total length (may be missing on chunked transfers)
    DWORD contentLen = 0;
    DWORD lenSz = sizeof(contentLen);
    WinHttpQueryHeaders(req,
        WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &contentLen, &lenSz, WINHTTP_NO_HEADER_INDEX);

    HANDLE hFile = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(sess);
        return false;
    }

    DWORD totalDownloaded = 0;
    int lastPct = -1;
    bool success = true;
    while (true) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) { success = false; break; }
        if (avail == 0) break;
        std::vector<char> buf(avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.data(), avail, &got)) { success = false; break; }
        DWORD wrote = 0;
        WriteFile(hFile, buf.data(), got, &wrote, NULL);
        totalDownloaded += got;
        if (contentLen > 0 && onPercent) {
            int pct = (int)((100ULL * totalDownloaded) / contentLen);
            if (pct != lastPct) {
                onPercent(pct);
                lastPct = pct;
            }
        }
    }

    CloseHandle(hFile);
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(sess);

    if (success && onPercent && lastPct != 100) onPercent(100);
    return success;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::wstring TempDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    if (n == 0) return L"C:\\Windows\\Temp";
    std::wstring s(buf, n);
    if (!s.empty() && s.back() != L'\\') s += L'\\';
    return s;
}

static std::wstring CurrentExePath() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    return buf;
}

// Extract the first browser_download_url for an asset whose name ends in .zip
static std::wstring FindZipAssetUrl(const std::wstring& body) {
    // body has multiple assets[] entries; for each, look for "browser_download_url"
    // and check that the surrounding "name" field ends in .zip.
    // Simple approach: find each browser_download_url, return the first that contains ".zip".
    std::wstring needle = L"\"browser_download_url\"";
    size_t pos = 0;
    while ((pos = body.find(needle, pos)) != std::wstring::npos) {
        std::wstring v = parsers::ExtractJsonStringField(body.substr(pos), L"browser_download_url");
        if (v.empty()) { pos += needle.size(); continue; }
        // case-insensitive .zip check
        std::wstring lower = v;
        for (auto& c : lower) c = (wchar_t)towlower(c);
        if (lower.size() >= 4 && lower.substr(lower.size() - 4) == L".zip") {
            return v;
        }
        pos += needle.size();
    }
    return L"";
}

// Find overlay.exe under a directory tree (max depth 3).
static std::wstring FindOverlayExe(const std::wstring& root, int depth = 0) {
    if (depth > 3) return L"";
    WIN32_FIND_DATAW fd;
    std::wstring pattern = root + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = root + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring rec = FindOverlayExe(full, depth + 1);
            if (!rec.empty()) { found = rec; break; }
        } else {
            std::wstring lower = name;
            for (auto& c : lower) c = (wchar_t)towlower(c);
            if (lower == L"overlay.exe") { found = full; break; }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void CleanupAfterRestart() {
    // Delete <self>.old left from a previous update
    std::wstring oldPath = CurrentExePath() + L".old";
    // Retry a few times in case the old process hasn't fully exited yet
    for (int i = 0; i < 30; ++i) {
        if (!DeleteFileW(oldPath.c_str())) {
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND) return;  // already gone — done
            Sleep(100);
        } else {
            return;
        }
    }
}

void CheckAndDownloadAsync(
    const std::string& releasesApiUrl,
    const std::wstring& currentVersion,
    std::function<void(const Status&)> onStatus)
{
    if (releasesApiUrl.empty()) return;

    std::thread([releasesApiUrl, currentVersion, onStatus]() {
        Status st;
        st.state = State::Checking;
        st.message = L"Checking for updates...";
        if (onStatus) onStatus(st);

        std::string body;
        if (!HttpsGetText(releasesApiUrl, body)) {
            st.state = State::Failed;
            st.message = L"Update check failed (network)";
            if (onStatus) onStatus(st);
            return;
        }

        // Parse tag_name
        std::wstring wbody = Utf8ToW(body);
        std::wstring tag = parsers::ExtractJsonStringField(wbody, L"tag_name");
        if (tag.empty()) {
            st.state = State::Failed;
            st.message = L"Update check failed (no tag in response)";
            if (onStatus) onStatus(st);
            return;
        }

        // Strip leading 'v'
        std::wstring tagNum = tag;
        if (!tagNum.empty() && (tagNum[0] == L'v' || tagNum[0] == L'V')) tagNum.erase(0, 1);

        if (!IsNewer(tagNum, currentVersion)) {
            st.state = State::Idle;
            st.tag = tag;
            st.message = L"Already up to date";
            if (onStatus) onStatus(st);
            return;
        }

        st.tag = tag;
        st.state = State::UpdateAvailable;
        st.message = L"Update available: " + tag;
        if (onStatus) onStatus(st);

        // Find the zip asset URL
        std::wstring assetUrlW = FindZipAssetUrl(wbody);
        if (assetUrlW.empty()) {
            st.state = State::Failed;
            st.message = L"No zip asset on the release";
            if (onStatus) onStatus(st);
            return;
        }
        std::string assetUrl = WToUtf8(assetUrlW);

        // Download
        std::wstring tempZip = TempDir() + L"aiov_update_" + tag + L".zip";
        st.state = State::Downloading;
        st.percent = 0;
        st.message = L"Downloading update " + tag + L"...";
        if (onStatus) onStatus(st);

        bool dlOk = HttpsDownloadToFile(assetUrl, tempZip, [&](int pct) {
            st.percent = pct;
            st.message = L"Updating " + tag + L": " + std::to_wstring(pct) + L"%";
            if (onStatus) onStatus(st);
        });

        if (!dlOk) {
            st.state = State::Failed;
            st.message = L"Download failed";
            if (onStatus) onStatus(st);
            return;
        }

        // Extract via built-in tar (Win10 1803+). Output goes to a directory
        // next to the zip.
        std::wstring extractDir = TempDir() + L"aiov_update_" + tag;
        CreateDirectoryW(extractDir.c_str(), NULL);

        std::wstring cmd = L"tar.exe -xf \"" + tempZip + L"\" -C \"" + extractDir + L"\"";
        STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(NULL, &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
        if (!ok) {
            st.state = State::Failed;
            st.message = L"Extract failed (tar not available)";
            if (onStatus) onStatus(st);
            return;
        }
        WaitForSingleObject(pi.hProcess, 60000);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        if (exitCode != 0) {
            st.state = State::Failed;
            st.message = L"Extract failed (tar exit " + std::to_wstring(exitCode) + L")";
            if (onStatus) onStatus(st);
            return;
        }

        // Find overlay.exe in the extracted tree
        std::wstring newExe = FindOverlayExe(extractDir);
        if (newExe.empty()) {
            st.state = State::Failed;
            st.message = L"New exe not found in update package";
            if (onStatus) onStatus(st);
            return;
        }

        // Stash the path for InstallAndRestart
        {
            std::lock_guard<std::mutex> lk(s_mutex);
            s_stagedNewExePath = newExe;
            s_stagedTag = tag;
        }

        st.state = State::Ready;
        st.message = tag + L" ready — press Ctrl+U to install";
        if (onStatus) onStatus(st);
    }).detach();
}

bool InstallAndRestart(std::function<void(const Status&)> onStatus) {
    std::wstring newExe, tag;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        newExe = s_stagedNewExePath;
        tag = s_stagedTag;
    }
    if (newExe.empty()) {
        if (onStatus) { Status st; st.state = State::Failed; st.message = L"Nothing staged"; onStatus(st); }
        return false;
    }

    Status st;
    st.state = State::Installing;
    st.tag = tag;
    st.message = L"Installing " + tag + L"...";
    if (onStatus) onStatus(st);

    std::wstring current = CurrentExePath();
    std::wstring oldPath = current + L".old";

    // If a .old already exists (from earlier failed update), try to remove it
    DeleteFileW(oldPath.c_str());

    // Rename current → current.old. This works on a running exe.
    if (!MoveFileExW(current.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        st.state = State::Failed;
        st.message = L"Could not rename current exe (locked?)";
        if (onStatus) onStatus(st);
        return false;
    }

    // Copy new exe into the current path
    if (!CopyFileW(newExe.c_str(), current.c_str(), FALSE)) {
        // Try to restore
        MoveFileExW(oldPath.c_str(), current.c_str(), MOVEFILE_REPLACE_EXISTING);
        st.state = State::Failed;
        st.message = L"Could not copy new exe into place";
        if (onStatus) onStatus(st);
        return false;
    }

    // Launch the new exe
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = L"\"" + current + L"\"";
    if (!CreateProcessW(NULL, &cmdline[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        st.state = State::Failed;
        st.message = L"Could not launch new exe";
        if (onStatus) onStatus(st);
        return false;
    }
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    // Tell the caller to exit
    if (onStatus) {
        Status done;
        done.state = State::Idle;
        done.message = L"Restarting...";
        onStatus(done);
    }
    return true;
}

} // namespace Updater
