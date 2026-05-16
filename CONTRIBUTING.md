# Contributing

Thanks for looking at this. The codebase is small, single-platform (Windows), and pure C++ / Win32 — no frameworks, no Python.

## Build

**Requirements:** MSYS2 with mingw-w64 g++ in PATH (`C:\msys64\mingw64\bin`).

```powershell
.\build_release.ps1
```

Produces `project_app\overlay.exe` (~4 MB). The script handles Windows Defender briefly locking the freshly-written file (retries up to 5 times).

## Code layout

| File | Responsibility |
|---|---|
| `main.cpp` | `WinMain`, GDI+ init, crash filter, dialog flow |
| `ConfigDialog.cpp/.h` | Welcome screen, provider picker, hotkey rebinder, tooltips |
| `ConfigLoader.cpp/.h` | `llm_config.txt` load/save, provider registry, theme palette, hotkey serialization |
| `OverlayWindow.cpp/.h` | Win32 window, message loop, rendering, hotkey handlers, polling timer, all send paths |
| `LLMClient.cpp/.h` | HTTPS streaming to Gemini / OpenAI-compat / Anthropic. SSE parsing. Error translation. |
| `AudioCapture.cpp/.h` | WASAPI loopback + optional mic, ring buffer (60s @ 16 kHz mono), WAV+base64, RMS for VAD |
| `Logger.cpp/.h` | File-based logging helper (`logs/app.log`) |
| `app.rc` + `app.ico` | Embedded icon + version metadata (linked via `windres`) |

`OverlayWindow.cpp` is the biggest file by a wide margin — splitting it is on the LOW roadmap but not done.

## Style

- Header style is loose; we match what's already there. No formal `.clang-format`.
- Avoid adding dependencies. We deliberately don't use STL JSON, fmt, libraries, etc. — Win32 + C++17 + WinHTTP + GDI/GDI+ only.
- Win32 GUIDs that require COM-link side-effects (`KSDATAFORMAT_SUBTYPE_*`, `PKEY_*`) are defined locally to keep the link line short.

## Testing

There is no automated test suite (on the LOW roadmap). Smoke test by:

1. Building cleanly via `build_release.ps1`
2. Launching `project_app\overlay.exe`, entering a Gemini key
3. Pressing INS (text), F8 (screen), F7 (audio), F9 (auto-mode) — confirm each round-trips
4. F10 to move the overlay, F11 to re-open settings without losing chat
5. Ctrl+R to reset, Ctrl+C to copy, Ctrl+F to search, Ctrl+E to export

## Sending PRs

- Keep changes focused. The HIGH roadmap items shipped as separate, reviewable chunks — please do the same.
- Run a smoke test before submitting.
- If your change adds a config field, update both `ConfigLoader::LoadConfig` and `SaveConfig`.
- If your change adds a hotkey, prefer a new `HotkeyAction` enum value (rebindable) over a fixed ID.

## License

MIT — see [LICENSE](./LICENSE).
