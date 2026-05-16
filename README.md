# Invisible AI Overlay

**A live interview & study copilot for Windows and macOS.** Captures system audio, screenshots, and clipboard text, sends them to an LLM, and streams answers into an overlay that's hidden from screen capture (Zoom / Meet / Discord / OBS / Teams).

Current version: **2.5.0**. Single executable on each platform — Win32 `.exe` (~4 MB) or macOS `.app` bundle (~330 KB binary, frameworks dynamically linked). No installer, no Python, no runtime.

> _Screenshots / demo GIF go here — see `docs/screenshots/` if/when added._

## Why this exists

In a live technical interview on a shared screen, you can:
- Press one hotkey to send the current screen to the AI and get a streamed answer (perfect for visible LeetCode problems)
- Press another to send the last 30s of meeting audio (the interviewer's spoken question)
- Flip on auto-answer mode — the app listens continuously, fires answers automatically when a substantive question is detected
- Use Claude / GPT-4o / Groq / etc. for the actual reasoning, with Gemini handling audio under the hood

The overlay window is invisible to screen capture:
- **Windows:** `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`
- **macOS:** `NSWindowSharingNone`

## Features

- **Hidden from screen capture** on both platforms
- **Click-through** when idle; mouse passes through to apps behind
- **Multi-provider**: Gemini, OpenAI, Anthropic, Groq, DeepSeek, OpenRouter, any OpenAI-compatible endpoint
- **Streaming responses** for all providers
- **Auto-answer mode** with VAD-gated polling (no API calls in silence) — Windows; coming to Mac in a future release
- **Bracket pair colorization** + keyword/string/number/comment syntax highlighting in code blocks (Windows; Mac uses a simpler theme-matched style for now)
- **Configurable hotkeys** (click-to-rebind on the welcome screen, persisted, conflict-detected) — both platforms
- **3 themes** (dark / light / high-contrast), adjustable opacity and font size
- **Live audio level dot** + **"thinking…"** indicator in the status bar
- **Persisted conversation** across runs, **named sessions**, export to markdown (Windows; Mac coming)
- **Crash logging** to `logs/crash.txt`, general log to `logs/app.log`

## Install

### Windows

1. Download `Interview-Hack-v2.5.0-windows.zip` from the [latest release](https://github.com/Zer0skillman/Interview-Hack/releases/latest)
2. Extract anywhere
3. Run `overlay.exe`

No installation, no dependencies, no registry writes. Settings live in `llm_config.txt` next to the exe.

### macOS

Requires **macOS 14 (Sonoma) or later** — the app uses `SCScreenshotManager` from ScreenCaptureKit, which is 14+.

1. Download `Interview-Hack-v2.5.0-macos.zip` from the [latest release](https://github.com/Zer0skillman/Interview-Hack/releases/latest)
2. Unzip and drag `overlay.app` into `/Applications` (or anywhere you like)
3. The app is **not code-signed** yet, so on first launch macOS will say "developer cannot be verified." Either right-click `overlay.app` → **Open**, or run once from Terminal:
   ```bash
   xattr -dr com.apple.quarantine /Applications/overlay.app
   open /Applications/overlay.app
   ```
4. **Grant Screen Recording permission** the first time you press the screenshot or audio hotkey — macOS will pop a system prompt, then you need to flip the toggle in **System Settings → Privacy & Security → Screen Recording** and relaunch the app.
5. If you enable the microphone option, you'll also get a **Microphone** prompt on first audio capture.

Settings live in `llm_config.txt` in the working directory (typically `~/llm_config.txt` when launched via `open`).

## Setup

On first run a welcome screen appears. Pick a provider, paste an API key, click **Start Overlay**. The Mac welcome screen also includes a click-to-rebind column for every hotkey.

| Provider | Where to get a key |
|---|---|
| Google Gemini (free tier) | https://aistudio.google.com/app/apikey |
| OpenAI | https://platform.openai.com/api-keys |
| Anthropic | https://console.anthropic.com/settings/keys |
| Groq | https://console.groq.com/keys |
| OpenRouter | https://openrouter.ai/keys |

> **Tip:** Even if you pick OpenAI/Anthropic/etc. for reasoning, paste a **Gemini fallback key** in the welcome dialog so that audio capture works — only Gemini accepts inline audio today.

## Hotkeys

All rebindable on the welcome screen. The defaults differ by platform because MacBook Airs lack Insert / Home / End / PgUp / PgDn keys and Mac function keys require Fn by default.

### Windows defaults

| Default key | Action |
|:---|:---|
| **F7** | Send last 30s of meeting audio |
| **F8** | Send screenshot of monitor under cursor |
| **F9** | Toggle auto-answer mode |
| **F10** | Move/resize mode (disables click-through) |
| **F11** | Open runtime settings |
| **F1** | About dialog |
| **F2** | Show/hide hotkey hints overlay |
| **INS** | Send clipboard text |
| **DEL** | Hide/show overlay |
| **END** | Exit |
| **PgUp / PgDn** | Scroll chat |
| **Ctrl+Alt+R** | Reset conversation |
| **Ctrl+Alt+C** | Copy last AI answer |
| **Ctrl+Alt+S** | Select-mode (click a bubble to copy / edit) |
| **Ctrl+Alt+G** | Regenerate last answer |
| **Ctrl+Alt+E** | Export chat to markdown |
| **Ctrl+Alt+F** | Search chat |
| **Ctrl+Alt+U** | Install staged update |
| **Ctrl+Alt+=** / **Ctrl+Alt+-** | Grow / shrink font |
| **Shift+← / →** | Scroll code blocks horizontally |

### macOS defaults

Mac keyboards don't have INS/DEL/END/HOME/PgUp/PgDn and F-keys need Fn, so we use Cmd+Option (⌘⌥) combos that work on every Mac keyboard.

| Default key | Action |
|:---|:---|
| **⌘⌥G** | Send screenshot ("Grab") |
| **⌘⌥A** | Send last 30s of meeting audio |
| **⌘⌥V** | Send clipboard text |
| **⌘⌥D** | Hide/show overlay ("Display") |
| **⌘⌥X** | Exit |
| **⌘⌥R** | Reset conversation |
| **⌘⌥C** | Copy last AI answer |
| **⌘⌥E** | Select-mode |
| **⌘⌥T** | Toggle auto-answer (UI only — full polling lands in a follow-up) |
| **⌘⌥W** | Move/resize mode |
| **⌘⌥K / ⌘⌥J** | Scroll chat up / down |
| **⌘⌥/** | Show/hide hotkey hints overlay |
| **⌘⌥,** | Open runtime settings |
| **⌘⌥I** | About dialog |

## Build (developers)

Both platforms share `CMakeLists.txt`. Windows additionally has a faster `build_release.ps1` script that bypasses CMake.

### Windows

**Requirements:** MSYS2 with mingw-w64 g++ in `C:\msys64\mingw64\bin`.

```powershell
.\build_release.ps1                      # fast path → project_app\overlay.exe
# or via CMake:
cmake -B build -G "MinGW Makefiles"
cmake --build build
```

### macOS

**Requirements:** Xcode Command Line Tools (`xcode-select --install`) — provides clang, CMake-compatible toolchain, and libcurl. Minimum macOS 14.

```bash
cmake -B build
cmake --build build
open build/overlay.app
```

See [CONTRIBUTING.md](./CONTRIBUTING.md) for architecture and code layout, and [MACOS_PORT.md](./MACOS_PORT.md) for Mac-specific design notes.

---

## FAQ

**Is it detectable?**
On Windows, `WDA_EXCLUDEFROMCAPTURE` excludes the overlay from standard screen-capture APIs (Zoom, Meet, Teams, Discord, OBS, Windows Game Bar, etc.). On macOS, `NSWindowSharingNone` does the same for the standard capture path. Lower-level proctoring tools that use mirror drivers, DRM-protected paths, or Apple's own ScreenCaptureKit-based recorders *may* still see it — this hasn't been tested against commercial proctoring suites.

**What data leaves my machine?**
Only when you press a hotkey or auto-answer fires: the text/audio/screenshot for that one call goes to your selected LLM provider over HTTPS. Nothing else is sent. There's no telemetry, no analytics, no background uploads.

**What about my microphone?**
Off by default. Only system audio (loopback) is captured. You can opt in via the "Also capture microphone" checkbox in the welcome screen.

**What does it cost to run?**
With Gemini Flash (the default):
- Pressing screen/audio hotkey a few times an hour: a few cents/hour
- Auto-answer running continuously with active conversation: ~$0.10-0.20/hr
- Idle (no speech): $0/hr (VAD gates the polling)

Claude, GPT-4o, etc. are significantly more expensive per token. Use them when you need their quality.

**The audio doesn't seem to be captured?**

- **Both platforms:** Watch the small green dot in the status bar — brighter = louder audio detected.
- **Windows:** Make sure audio is actually playing through your selected output device. WASAPI loopback follows the current default render device — unplug/replug headphones mid-session and you may need to restart the overlay.
- **macOS:** Confirm you granted **Screen Recording** permission in System Settings → Privacy & Security and **fully relaunched** the app after granting. ScreenCaptureKit silently produces no audio if the permission isn't granted.

**My API key isn't working?**
First call will surface a clear error ("401 Unauthorized — check your API key" / "403 Forbidden" / etc.). If you keep getting 404, double-check the model name — providers rename them often.

**Where are my settings?**
- **Windows:** `llm_config.txt` next to `overlay.exe`.
- **macOS:** `llm_config.txt` in the working directory. When you `open overlay.app`, the working directory is your home folder — so it lands at `~/llm_config.txt`.
  Plain text on both platforms, edit freely. Chat history is in `chat.<sessionname>.txt`.

**Can I run it on Linux?**
Not currently. The `IAudioCapture` / `IScreenshot` / `HttpClient` interfaces are platform-agnostic, so a Linux impl is technically possible (PipeWire / PulseAudio for loopback, X11 / Wayland for screenshots) — but nobody's writing it. PRs welcome.

---

## Troubleshooting

### Windows

| Symptom | Likely cause |
|---|---|
| Welcome screen never appears | Defender or antivirus may have quarantined the exe — check quarantine. The exe isn't code-signed yet. |
| `overlay.exe` won't deploy from build | Another `overlay.exe` is running. Press END in the running overlay, or kill the process. |
| F8 sends but no audio attached | Last 30s of speakers had no audio. Check the green level dot. |
| Auto-answer fires on small talk | The classifier is too loose for your situation. Open an issue with the offending transcript. |
| Auto-answer misses real questions | Conversely too strict. Same — open an issue. |
| "Stream cut off — network error" | Transient network drop. Press Ctrl+Shift+R to regenerate, or just re-send. |
| Move mode banner stuck on | Press F10 again to exit. |

### macOS

| Symptom | Likely cause |
|---|---|
| "developer cannot be verified" on first launch | The `.app` isn't signed yet. Right-click → **Open**, or `xattr -dr com.apple.quarantine overlay.app`. |
| Screenshot hotkey fires but the AI sees a black image | Screen Recording permission isn't granted. System Settings → Privacy & Security → Screen Recording → toggle on, then **relaunch the app**. |
| Audio hotkey fires but the AI says it heard silence | Same — Screen Recording is also what gates ScreenCaptureKit's audio loopback. |
| Hotkeys do nothing | Check `~/logs/app.log` — each registration logs success or failure. Another running app may have claimed the same global hotkey. Press ⌘⌥, to open settings and rebind. |
| "operation not permitted" / mic doesn't work | Microphone permission missing. System Settings → Privacy & Security → Microphone → toggle on. |
| F7/F8 don't fire (you migrated from old config) | Mac Air has no F-keys without Fn. Open ⌘⌥, → click "Reset hotkeys to Mac defaults" — you'll get the working ⌘⌥letter set. |

Check `logs/app.log` and `logs/crash.txt` (if exists) for diagnostic info. On macOS those land in your home folder (`~/logs/`).

---

## License

[MIT](./LICENSE).
