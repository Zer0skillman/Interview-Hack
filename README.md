# Invisible AI Overlay

**A live interview & study copilot for Windows.** Captures system audio, screenshots, and clipboard text, sends them to an LLM, and streams answers into an overlay that's hidden from screen capture (Zoom / Meet / Discord / OBS).

Current version: **2.3.0**. Single executable, ~4 MB, pure C++ / Win32. No Python, no runtime, no installer required.

> _Screenshots / demo GIF go here — see `docs/screenshots/` if/when added._

## Why this exists

In a live technical interview on a shared screen, you can:
- Press **F8** to send the current screen to the AI and get a streamed answer (perfect for visible LeetCode problems)
- Press **F7** to send the last 30s of meeting audio (the interviewer's spoken question)
- Press **F9** to flip on auto-answer mode — the app listens continuously, fires answers automatically when a substantive question is detected
- Use Claude / GPT-4o / Groq / etc. for the actual reasoning, with Gemini handling audio under the hood

The overlay is hidden from `WDA_EXCLUDEFROMCAPTURE` so it doesn't appear on screen-shared video.

## Features

- **Hidden from screen capture** via `SetWindowDisplayAffinity`
- **Click-through** when idle; mouse passes through to apps behind
- **Multi-provider**: Gemini, OpenAI, Anthropic, Groq, DeepSeek, OpenRouter, any OpenAI-compatible endpoint
- **Streaming responses** for all providers
- **Auto-answer mode** with VAD-gated polling (no API calls in silence)
- **Bracket pair colorization** + keyword/string/number/comment syntax highlighting in code blocks
- **Configurable hotkeys** (click-to-rebind, persisted, conflict-detected)
- **3 themes** (dark / light / high-contrast), adjustable opacity and font size
- **Persisted conversation** across runs, **named sessions**, export to markdown
- **Chat search** (Ctrl+F), **regenerate** (Ctrl+Shift+R), **per-message copy** (Ctrl+Shift+C)
- **Move/resize the overlay** (F10), remembered across runs
- **Crash logging** to `logs/crash.txt`, general log to `logs/app.log`

## Install

1. Download the latest `project_app.zip`
2. Extract anywhere
3. Run `overlay.exe`

No installation, no dependencies, no registry writes. Settings live in `llm_config.txt` next to the exe.

## Setup

On first run a welcome screen appears. Pick a provider, paste an API key, click **Start Overlay**.

| Provider | Where to get a key |
|---|---|
| Google Gemini (free tier) | https://aistudio.google.com/app/apikey |
| OpenAI | https://platform.openai.com/api-keys |
| Anthropic | https://console.anthropic.com/settings/keys |
| Groq | https://console.groq.com/keys |
| OpenRouter | https://openrouter.ai/keys |

## Hotkeys

All rebindable on the welcome screen except the fixed shortcuts.

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
| **Ctrl+R** | Reset conversation |
| **Ctrl+C** | Copy last AI answer |
| **Ctrl+Shift+C** | Select-mode (click a bubble to copy it) |
| **Ctrl+Shift+R** | Regenerate last answer |
| **Ctrl+E** | Export chat to markdown |
| **Ctrl+F** | Search chat |
| **Ctrl+=** / **Ctrl+-** | Grow / shrink font |
| **Shift+← / →** | Scroll code blocks horizontally |

## Build (developers)

**Requirements:** MSYS2 with mingw-w64 g++ in `C:\msys64\mingw64\bin`.

```powershell
.\build_release.ps1
```

See [CONTRIBUTING.md](./CONTRIBUTING.md) for architecture and code layout.

---

## FAQ

**Is it detectable?**
`SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` excludes the overlay from standard screen-capture APIs (Zoom, Meet, Teams, Discord, OBS, Windows Game Bar, etc.). Lower-level proctoring tools that use mirror drivers or DRM-protected paths *may* still see it — this hasn't been tested against commercial proctoring suites.

**What data leaves my machine?**
Only when you press a hotkey or auto-answer fires: the text/audio/screenshot for that one call goes to your selected LLM provider over HTTPS. Nothing else is sent. There's no telemetry, no analytics, no background uploads.

**What about my microphone?**
Off by default. Only system audio (loopback) is captured. You can opt in via the "Also capture microphone" checkbox in the welcome screen.

**What does it cost to run?**
With Gemini Flash (the default):
- Pressing F8/F7 a few times an hour: a few cents/hour
- Auto-answer (F9) running continuously with active conversation: ~$0.10-0.20/hr
- Idle (no speech): $0/hr (VAD gates the polling)

Claude, GPT-4o, etc. are significantly more expensive per token. Use them when you need their quality.

**The audio doesn't seem to be captured?**
- Make sure audio is actually playing through your speakers (or whatever output device you selected)
- Check the small green dot in the transcript bar — brighter = louder audio detected
- If you have multiple output devices, pick the right one in the welcome screen
- WASAPI loopback follows your *current* default render device. Unplug/replug headphones mid-session and you may need to restart the overlay.

**My API key isn't working?**
Welcome screen → first call will surface a clear error ("401 Unauthorized — check your API key" / "403 Forbidden" / etc.). If you keep getting 404, double-check the model name — providers rename them often.

**Where are my settings?**
`llm_config.txt` next to `overlay.exe`. Plain text, edit freely. Chat history is in `chat.<sessionname>.txt`.

**Can I run it on macOS / Linux?**
No. This is Windows-specific (Win32, WASAPI, GDI+). Cross-platform is on the roadmap as an explicit future-only item.

---

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Welcome screen never appears | Defender or antivirus may have quarantined the exe — check quarantine. The exe isn't code-signed yet. |
| `overlay.exe` won't deploy from build | Another `overlay.exe` is running. Press END in the running overlay, or kill the process. |
| F8 sends but no audio attached | Last 30s of speakers had no audio. Check the green level dot. |
| Auto-answer fires on small talk | The classifier is too loose for your situation. Open an issue with the offending transcript. |
| Auto-answer misses real questions | Conversely too strict. Same — open an issue. |
| "Stream cut off — network error" | Transient network drop. Press Ctrl+Shift+R to regenerate, or just re-send. |
| Move mode banner stuck on | Press F10 again to exit. |

Check `logs/app.log` and `logs/crash.txt` (if exists) for diagnostic info.

---

## License

[MIT](./LICENSE).
