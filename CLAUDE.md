# Project context for Claude Code

This file is auto-loaded by Claude Code on every session. Don't make it long.
Detailed docs are in `ROADMAP.md`, `CONTRIBUTING.md`, `MACOS_PORT.md`.

## What this is

**Invisible AI Overlay** — Windows desktop overlay for live coding interviews
and study sessions. Captures system audio (WASAPI loopback), screenshots, and
clipboard text; sends to an LLM (Gemini/Claude/GPT-4o/Groq/etc.); streams the
answer into a chat overlay that's hidden from screen capture
(`SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`).

Current version: **2.5.0** (or whatever's in `app.rc`). Repo:
<https://github.com/Zer0skillman/Interview-Hack>. **macOS port is live**
(phase 2 landed) — both Windows and macOS build from this repo.

## File map (high level)

| Purpose | Files |
|---|---|
| Cross-platform interfaces | `IAudioCapture.h`, `IScreenshot.h`, `HttpClient.h` |
| Windows impls | `AudioCapture.cpp`, `Screenshot_Win.cpp`, `OverlayWindow.cpp`, `Overlay_Rendering.cpp`, `ConfigDialog.cpp`, `main.cpp`, `HttpClient_Win.cpp` |
| macOS impls | `macos/MacAudioCapture.mm`, `macos/Screenshot_Mac.mm`, `macos/MacOverlayWindow.mm` (+`.h`), `macos/MacRenderer.mm`, `macos/MacConfigDialog.mm`, `macos/main_mac.mm`, `macos/HttpClient_Mac.mm`, `macos/Info.plist` |
| Cross-platform business | `LLMClient.cpp`, `ConfigLoader.cpp`, `Logger.cpp`, `Updater.cpp`, `Parsers.h`, `WinCompat.h` (stubs MOD_*/VK_*/RGB on non-Windows) |
| Tests | `tests.cpp` (parsers only) — separate exe, 27 checks |
| Build | `build_release.ps1` (Windows-only fast path), `CMakeLists.txt` (cross-platform), `.github/workflows/build.yml` (CI runs both Windows + macOS) |
| macOS port docs | `MACOS_PORT.md` — phase 2 is now in the repo; still useful for understanding the design |

## Build

```powershell
.\build_release.ps1                # Windows, fast iteration, produces project_app\overlay.exe
cmake -B build && cmake --build build  # cross-platform — Windows or macOS
                                       # On macOS, produces build/overlay.app
```

CI exercises both paths on every push.

## Conventions

- **All Ctrl+letter shortcuts are global hotkeys** via `RegisterHotKey`. They
  hijack the system shortcut in EVERY app. Bare Ctrl+letter is forbidden as a
  default; use `Ctrl+Alt+*` combos. See the v2.4.3 commit for the painful
  lesson where Ctrl+C broke text copying everywhere.
- **Versions bumped in 4-5 places per release** — `app.rc` (FILEVERSION + the
  two version strings), `main.cpp` (logger + crash filter), `OverlayWindow.cpp`
  (About dialog + update-check baseline), `LLMClient.cpp` (user-agents).
  CMake project version too.
- **Tags trigger a GitHub Release** via the workflow. `v*` tag push -> CI
  builds + zips `project_app/` -> creates Release with the zip attached.
- **Auto-updater** check is real and works end-to-end. Default URL points at
  this repo's `/releases/latest`. Users get a notice in the transcript bar
  + Ctrl+Alt+U to install (rename-trick self-replace).
- **Leaked Gemini API key** in old git history at
  `AIzaSyDfUjzN9eBoi2ZJb4VUc-AzokIFiNAYfbM`. Welcome screen warns if user
  is still on it. Should be rotated; can't be removed from git history
  without a force-rewrite.

## Working with this user

- **Execute when delegated.** When user says "best/recommend/you decide" or
  "do it all", they want momentum, not clarifying questions. Skip
  `AskUserQuestion`. Do the work; explain choices in the summary.
- **Tag and push are expected after most user-visible changes.** They want
  to see releases in GitHub. No-tag commits are for pure infrastructure
  (CI tweaks, refactors with no behavior change).
- **Be honest about what you can't do.** When something needs a Mac, says
  it. When a refactor is theater, say so. Don't push speculative code.

## If you're picking up the macOS port

Phase 2 is in. Both platforms build from this repo. The macOS-specific
behaviors worth knowing:

- **Defaults differ.** On Mac, the default hotkeys are `Cmd+Option+G/A/V/D/X/…`
  (Mac Airs have no Insert/Forward-Delete/Home/End/PgUp/PgDn keys and the
  F-row needs Fn unless the user changes System Settings). On Windows the
  defaults remain F7/F8/INS/DEL/END/PgUp/PgDn. Both sets coexist via
  `#ifdef __APPLE__` in `ConfigLoader::DefaultHotkeys()`.
- **`BindingToString`/`BindingFromString` accept both label forms** —
  saved config uses `Cmd+`/`Option+` on Mac and `Win+`/`Alt+` on Windows;
  parser accepts either so a file moved between platforms still loads.
- **Mac-only hardcoded extras**: `Cmd+Option+/` (toggle hints panel),
  `Cmd+Option+,` (reopen settings dialog), `Cmd+Option+I` (about). Not in
  the `HotkeyAction` enum — registered separately in `MacOverlayWindow`'s
  `Initialize` mirroring how F2/F11/F1 are hardcoded on Windows.
- **macOS 14+ only.** `CGDisplayCreateImage` was removed in 15;
  `SCScreenshotManager` (the replacement) is 14+. Don't lower this.
- **ARC is enabled per-source** in CMake via `-fobjc-arc` on the .mm files.
  `__bridge_retained`/`__bridge_transfer` actually transfer ownership.
- **`Logger`**: writes to `logs/app.log` in CWD. When launched via
  `open build/overlay.app` the CWD is the user's home, so logs land at
  `~/logs/app.log`.
