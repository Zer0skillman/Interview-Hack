#pragma once
#include <string>
#include <functional>

// Self-updater. Talks to a GitHub Releases /latest endpoint, downloads the
// release zip, extracts via the built-in `tar -xf`, and swaps the current
// overlay.exe with the new one using the rename trick (Windows lets you
// rename a running exe but not overwrite it). After the swap, the old exe
// PostQuitMessage()s and the freshly-launched new exe takes over.
//
// All public methods are safe to call from any thread; progress callbacks fire
// on the calling thread.
namespace Updater {

enum class State {
    Idle,            // no update activity
    Checking,        // initial GET to /releases/latest
    UpdateAvailable, // newer tag found; not yet downloaded
    Downloading,     // streaming the asset
    Ready,           // downloaded + extracted; waiting for user to install
    Installing,      // doing the file swap
    Failed,          // error occurred (see message)
};

struct Status {
    State state = State::Idle;
    int percent = 0;          // 0..100 during Downloading
    std::wstring tag;         // remote tag (e.g. "v2.4.1") when known
    std::wstring message;     // human-readable status / error
};

// Kick off the full update flow (check → download → ready). Runs on a worker
// thread. onStatus fires with progress; safe to call from worker — caller
// is responsible for thread-safe handoff to UI.
void CheckAndDownloadAsync(
    const std::string& releasesApiUrl,
    const std::wstring& currentVersion,   // e.g. L"2.4.1"
    std::function<void(const Status&)> onStatus);

// Once Status::Ready, call this to do the actual swap and relaunch.
// Returns false on failure; caller should exit anyway if true.
bool InstallAndRestart(std::function<void(const Status&)> onStatus);

// Cleanup the .old file left by a previous update. Call once at startup.
void CleanupAfterRestart();

} // namespace Updater
