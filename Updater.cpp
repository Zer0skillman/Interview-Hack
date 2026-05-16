#include "Updater.h"
#include "HttpClient.h"
#include "Parsers.h"  // ExtractJsonStringField

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace Updater {

static std::mutex s_mutex;
static std::wstring s_stagedNewExePath;
static std::wstring s_stagedTag;

// ---------------------------------------------------------------------------
// Tiny portable UTF-8 conversions (same logic as LLMClient's)
// ---------------------------------------------------------------------------

static std::string WToUtf8(const std::wstring& w) {
    std::string out;
    out.reserve(w.size() * 2);
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t cp = (uint32_t)w[i];
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
            uint32_t low = (uint32_t)w[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

static std::wstring Utf8ToW(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char b = (unsigned char)s[i];
        uint32_t cp = 0; int extra = 0;
        if (b < 0x80) { cp = b; extra = 0; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; extra = 1; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; extra = 2; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; extra = 3; }
        else { i++; continue; }
        ++i;
        for (int k = 0; k < extra; ++k) {
            if (i >= s.size()) { cp = 0; break; }
            cp = (cp << 6) | ((unsigned char)s[i] & 0x3F); ++i;
        }
        if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
            cp -= 0x10000;
            out += (wchar_t)(0xD800 + (cp >> 10));
            out += (wchar_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (wchar_t)cp;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Version comparison
// ---------------------------------------------------------------------------

static int ParseVersionPart(const std::wstring& v, int idx) {
    int a, b, c;
    a = b = c = 0;
    swscanf(v.c_str(), L"%d.%d.%d", &a, &b, &c);
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
// Platform-specific helpers
// ---------------------------------------------------------------------------

static std::wstring TempDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    if (n == 0) return L"C:\\Windows\\Temp\\";
    std::wstring s(buf, n);
    if (!s.empty() && s.back() != L'\\') s += L'\\';
    return s;
#else
    const char* t = std::getenv("TMPDIR");
    std::string s = t ? t : "/tmp/";
    if (s.empty() || s.back() != '/') s += '/';
    return Utf8ToW(s);
#endif
}

static std::wstring CurrentExePath() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    return buf;
#else
    // On Mac the bundle path is what matters for install, not the inner
    // executable. For now return the executable path; install is disabled
    // on Mac anyway.
    return L"";
#endif
}

// Look for the first browser_download_url that ends in .zip
static std::wstring FindZipAssetUrl(const std::wstring& body) {
    std::wstring needle = L"\"browser_download_url\"";
    size_t pos = 0;
    while ((pos = body.find(needle, pos)) != std::wstring::npos) {
        std::wstring v = parsers::ExtractJsonStringField(body.substr(pos), L"browser_download_url");
        if (v.empty()) { pos += needle.size(); continue; }
        std::wstring lower = v;
        for (auto& c : lower) c = (wchar_t)towlower(c);
        if (lower.size() >= 4 && lower.substr(lower.size() - 4) == L".zip") {
            return v;
        }
        pos += needle.size();
    }
    return L"";
}

#ifdef _WIN32
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
#endif

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void CleanupAfterRestart() {
#ifdef _WIN32
    std::wstring oldPath = CurrentExePath() + L".old";
    for (int i = 0; i < 30; ++i) {
        if (!DeleteFileW(oldPath.c_str())) {
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND) return;
            Sleep(100);
        } else {
            return;
        }
    }
#endif
    // No-op on macOS — there's no rename-trick equivalent for .app bundles
    // and our installer doesn't replace the running app in-place.
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

        std::vector<std::string> headers = {
            "Accept: application/json",
            "User-Agent: AIOverlay-Updater",
        };
        HttpResponse r = HttpClient::Get(releasesApiUrl, headers);
        if (!r.errorMsg.empty() || r.body.empty()) {
            st.state = State::Failed;
            st.message = L"Update check failed (network)";
            if (onStatus) onStatus(st);
            return;
        }

        std::wstring wbody = Utf8ToW(r.body);
        std::wstring tag = parsers::ExtractJsonStringField(wbody, L"tag_name");
        if (tag.empty()) {
            st.state = State::Failed;
            st.message = L"Update check failed (no tag in response)";
            if (onStatus) onStatus(st);
            return;
        }

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

#ifdef _WIN32
        std::wstring assetUrlW = FindZipAssetUrl(wbody);
        if (assetUrlW.empty()) {
            st.state = State::Failed;
            st.message = L"No zip asset on the release";
            if (onStatus) onStatus(st);
            return;
        }
        std::string assetUrl = WToUtf8(assetUrlW);

        std::wstring tempZip = TempDir() + L"aiov_update_" + tag + L".zip";
        st.state = State::Downloading;
        st.percent = 0;
        st.message = L"Downloading update " + tag + L"...";
        if (onStatus) onStatus(st);

        bool dlOk = HttpClient::Download(assetUrl, WToUtf8(tempZip), [&](int pct) {
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

        std::wstring newExe = FindOverlayExe(extractDir);
        if (newExe.empty()) {
            st.state = State::Failed;
            st.message = L"New exe not found in update package";
            if (onStatus) onStatus(st);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(s_mutex);
            s_stagedNewExePath = newExe;
            s_stagedTag = tag;
        }

        st.state = State::Ready;
        st.message = tag + L" ready — press the update hotkey to install";
        if (onStatus) onStatus(st);
#else
        // macOS: notify but don't auto-install. The .app bundle replacement
        // story needs a helper script and is best left to a future change.
        st.state = State::UpdateAvailable;
        st.message = L"Update " + tag + L" available — download from GitHub Releases.";
        if (onStatus) onStatus(st);
#endif
    }).detach();
}

bool InstallAndRestart(std::function<void(const Status&)> onStatus) {
#ifdef _WIN32
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

    DeleteFileW(oldPath.c_str());

    if (!MoveFileExW(current.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        st.state = State::Failed;
        st.message = L"Could not rename current exe (locked?)";
        if (onStatus) onStatus(st);
        return false;
    }

    if (!CopyFileW(newExe.c_str(), current.c_str(), FALSE)) {
        MoveFileExW(oldPath.c_str(), current.c_str(), MOVEFILE_REPLACE_EXISTING);
        st.state = State::Failed;
        st.message = L"Could not copy new exe into place";
        if (onStatus) onStatus(st);
        return false;
    }

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

    if (onStatus) {
        Status done;
        done.state = State::Idle;
        done.message = L"Restarting...";
        onStatus(done);
    }
    return true;
#else
    if (onStatus) {
        Status st;
        st.state = State::Failed;
        st.message = L"Auto-install not supported on macOS — download from GitHub.";
        onStatus(st);
    }
    return false;
#endif
}

} // namespace Updater
