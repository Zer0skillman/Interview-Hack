# Invisible AI Overlay — Project Status & Roadmap

**Current version:** 2.3.0

Open work items live in section 5. Section 3 lists what currently works.

---

## 1. What it is

Windows desktop overlay that helps you during live coding interviews and study sessions:

- Hidden from screen capture (Zoom / Meet / OBS / Discord screen-share) via `SetWindowDisplayAffinity`
- Captures **system audio** (WASAPI loopback — what others say in the meeting), **screenshots** (monitor under cursor), and **clipboard text**
- Sends to an LLM and streams the answer into a chat overlay
- Multiple providers: Gemini (default, only one with audio), Claude, GPT-4o, Groq, DeepSeek, OpenRouter, custom (OpenAI-compatible)
- Auto-answer mode: continuously listens, fires answers automatically when a substantive question is detected

---

## 2. Architecture

Single executable, ~3.8 MB, pure C++ / Win32:

| File | Responsibility |
|---|---|
| `main.cpp` | `WinMain`, GDI+ init, dialog flow |
| `ConfigDialog.cpp/.h` | Welcome screen, provider picker, hotkey rebinder |
| `ConfigLoader.cpp/.h` | Load/save `llm_config.txt`, provider registry, hotkey serialization |
| `OverlayWindow.cpp/.h` | Win32 window, message loop, rendering, all hotkey handlers, polling timer |
| `LLMClient.cpp/.h` | HTTPS streaming to Gemini / OpenAI-compat / Anthropic. SSE parsing. |
| `AudioCapture.cpp/.h` | WASAPI loopback, ring buffer (60s @ 16 kHz mono), WAV+base64 encoding, RMS for VAD |
| `build_release.ps1` | g++ build + dist packaging |

---

## 3. Feature inventory — what works today

### Capture
- ✅ Screenshot of monitor under cursor (BitBlt + GDI+ PNG → base64)
- ✅ WASAPI loopback audio (60s ring buffer, mono 16 kHz)
- ✅ Clipboard text
- ✅ Optional microphone input (off by default, checkbox in welcome screen, mixed additively into the ring buffer at 70% gain)
- ✅ **Per-device selection** — dropdowns for output (loopback) and microphone capture device in the welcome screen. Defaults to system default. Falls back to default if the saved device is no longer present.

### Hotkeys (rebindable except system keys)
| Default key | Action | Rebindable |
|---|---|---|
| `F7` | Send last 30s audio only | ✅ |
| `F8` | Send screenshot only | ✅ |
| `F9` | Toggle auto-answer mode | ✅ |
| `F10` | Move/resize mode | ✅ |
| `Ctrl+R` | Reset conversation | ✅ |
| `Ctrl+C` | Copy last AI answer | ✅ |
| `Ctrl+Shift+C` | Select-mode: click any chat bubble to copy it | ✅ |
| `Ctrl+Shift+R` | **Regenerate last AI answer** (re-fires the last question) | fixed |
| `INS` | Send clipboard text | ✅ |
| `DEL` | Hide / show overlay | ✅ |
| `END` | Exit | ✅ |
| `PgUp` / `PgDn` | Scroll chat | ✅ |
| `Ctrl+E` | Export chat to markdown (opens the file) | fixed |
| `Ctrl+F` | Open chat search bar | fixed |
| `Ctrl+=` / `Ctrl+-` | Grow / shrink chat font (persisted) | fixed |
| `Shift+←` / `Shift+→` | Scroll code blocks horizontally | fixed |
| `F1` | About dialog | fixed |
| `F11` | Open runtime settings | fixed |

Rebinding UI in the welcome screen has a **"Reset to defaults"** button.

### Providers
- ✅ Google Gemini (text + image + **audio**)
- ✅ OpenAI (GPT-4o, GPT-4o mini, o1, GPT-5) — text + image
- ✅ Anthropic Claude (Opus 4.7, Sonnet 4.6, Haiku 4.5) — text + image
- ✅ Groq (Llama / Mixtral)
- ✅ DeepSeek
- ✅ OpenRouter (aggregator)
- ✅ Custom OpenAI-compatible (Ollama, LM Studio, etc.)
- ✅ **Provider dropdown labels Gemini as "audio supported"**
- ✅ **Gemini audio fallback**: when a non-Gemini provider is selected, F7/audio calls auto-route to Gemini using a stored "Gemini key for audio" field (so you can use Claude for everything else)
- All providers stream via SSE

### UI
- ✅ Dark themed welcome screen with branding + tagline
- ✅ Configurable hotkeys (click to rebind, Esc to cancel, persisted to config)
- ✅ Hotkey conflict detection — duplicate bindings rejected on save
- ✅ **"Reset to defaults" button** in the hotkey panel
- ✅ Bracket pair colorization in code blocks (rainbow: yellow → pink → cyan → green)
- ✅ Monospace code rendering, no word-wrap (preserves code lines as written)
- ✅ **Horizontal code scroll** via `Shift+Left/Right` for long lines
- ✅ Live transcript bar at bottom with audio level dot + AUTO ON/OFF badge
- ✅ **In-flight indicator** — solid orange dot in transcript bar while any API call is running
- ✅ State-aware transcript label: shows "(press F9 to enable auto-answer)" when off, "(listening...)" / "(listening — no speech)" when on
- ✅ Move/resize mode (F10) with red banner indicator
- ✅ **Position/size persists across runs** — saved to llm_config.txt on every move/resize, restored on launch (clamped to screen bounds)
- ✅ **Runtime settings dialog (F11)** — re-open welcome dialog mid-session without losing chat; changes apply live (hotkeys re-registered, audio restarted if mic toggle changed)
- ✅ **Smart auto-scroll** — only snaps to bottom if user was already at the bottom; if scrolled up reading, streaming chunks won't jerk them back
- ✅ Hidden from screen capture
- ✅ Click-through outside move/select mode
- ✅ **Select mode** (Ctrl+Shift+C): blue banner appears, click-through disabled, click any chat bubble to copy that message's text to clipboard. Strips `[tag]` prefixes from user bubbles. Esc cancels. Auto-exits after copy.
- ✅ "Clear" button on API Key field + visible warning if the historically-leaked key is in use
- ✅ API key fields masked (ES_PASSWORD)

### Auto-answer
- ✅ VAD via 2s RMS energy threshold
- ✅ **Polling fully gated on F9** — zero API calls when auto mode is off
- ✅ Adaptive polling: 3s when speech detected, 8s in silence
- ✅ Two-stage: cheap classifier decides "is this a substantive question", separate streaming call generates the answer
- ✅ Dedupe on question-text prefix

### Reliability
- ✅ HTTPS timeouts on all paths: 5s resolve, 10s connect, 30s send, 60s receive
- ✅ Stream-cut-off detection: "[stream cut off — network error]" appended if SSE breaks mid-response
- ✅ Actionable error messages: HTTP 401/403/404/429/500/503 mapped to plain-English text; WinHTTP errors (timeout, DNS, TLS) similarly mapped
- ✅ Conversation history cap (default 200 messages, configurable) — oldest pairs trimmed when exceeded
- ✅ **Crash logging**: unhandled exceptions write `logs/crash.txt` with timestamp, exception code, faulting address

### Sessions & export
- ✅ Conversation persists across runs (`chat.<session>.txt`, gated by `restore_session` config). Saved on exit, loaded on launch. Wiped by Ctrl+R.
- ✅ **Named sessions** — `session_name` field in welcome screen. Per-session chat files (`chat.<name>.txt`). Switching in F11 settings saves current under old name and loads new.
- ✅ Markdown export via `Ctrl+E` → `chat_export_YYYYMMDD_HHMM.md`, auto-opens in default markdown viewer
- ✅ Chat search: `Ctrl+F` opens a search bar at the top; type to filter, Enter scrolls to first match, Esc closes
- ✅ **Regenerate last answer** (`Ctrl+Shift+R`) — drops last bot reply, re-fires the same question

### Appearance & personalization
- ✅ 3 built-in themes (`dark` default, `light`, `contrast`) — all colors driven by a `ThemeColors` struct
- ✅ Adjustable opacity (60-255 alpha, persisted as `opacity_alpha`)
- ✅ Adjustable font size for prose and code (Ctrl+= / Ctrl+- to grow/shrink, persisted)
- ✅ Optional per-message timestamps (`show_timestamps` config) — dim HH:MM in top-right corner of bubble
- ✅ Code block language label — language identifier after the opening ``` rendered as accent-colored tag
- ✅ Token meter in the transcript bar
- ✅ About dialog (F1)
- ✅ **Syntax highlighting in code blocks**: keywords (purple), strings (orange), numbers (cyan), comments (dim gray-green), brackets (rainbow pair-coloring). Pan-language keyword set covers C-family, Python, JS, Rust, Java.
- ✅ **Inline markdown stripping** in prose — `**bold**`, `*italic*`, `_underscore_`, and `` `code` `` markers are stripped from display so prose reads cleanly (the originals stay in history for re-send). Full styled rendering (actual bold/italic fonts) deferred — see deferrals below.
- ✅ **Sound on auto-answer** — optional, gated by `sound_on_auto` config (default off). `MessageBeep(MB_OK)` when an auto-answer fires.

### Help & docs
- ✅ **Tooltips on welcome dialog** — hover help on every major control
- ✅ **F2 hotkey hints overlay** — translucent panel listing all current bindings + fixed shortcuts
- ✅ **README** with FAQ + troubleshooting + provider key links
- ✅ **CONTRIBUTING.md** + **LICENSE** (MIT full text)
- ✅ **File-based logger** at `logs/app.log` (info/warn/error with timestamps)
- ✅ **Optional update check** — set `update_check_url` in config to a GitHub Releases API URL; on launch we fetch, compare `tag_name` to embedded version, show a one-shot notice if newer. Disabled by default.

### Distribution
- ✅ Embedded application icon (PNG-in-ICO, "AI" mark on blue)
- ✅ Version + product metadata in `overlay.exe` (FileVersion 2.3.0.0, ProductName, FileDescription, LegalCopyright) — visible in Windows file properties
- ✅ **GitHub Actions build CI** (`.github/workflows/build.yml`) — builds on push/PR, uploads `project_app/` as artifact

---

## 4. Behavioral quirks worth knowing (not bugs, but call them out)

- **Polling fires when F9 is OFF** (just for the transcript bar). Costs ~$0.10/hr on Gemini Flash. Intentional but wasteful — see `[Critical] Gate polling on F9`.
- **F8 is screenshot-only now.** Combined screen+audio is only done via auto mode (F9) — by design after the F8/F7 split.
- **Audio dropped silently for non-Gemini providers.** Only Gemini accepts raw audio. F7 will show a chat message if you press it on Claude/OpenAI, but auto-mode polls just no-op.
- **Loopback follows the default audio device.** If you unplug headphones mid-session and Windows switches output, the capture thread may need a restart. Untested.
- **Conversation history grows unbounded** in memory until app exit or `Ctrl+R`.

---

## 5. Open work items

Real code tasks you can pick up. Sized so each fits in a focused session.

| Item | What it gets you | Effort |
|---|---|---|
| **Styled markdown rendering** (real bold/italic, not just stripping) | `**bold**` and `*italic*` render with bold/italic Segoe UI runs in prose. Needs manual word-wrap with mixed-font runs since `DrawText` doesn't handle that. | ~3 h |
| **Per-message edit-and-resend** | Right-click (in select mode) a user bubble → edit text in an inline popup → resend, replacing the bot reply. Needs a small modal edit dialog + history-slicing. | ~3 h |
| **Streaming auto-answer single-call merge** | Combine the classifier + answer into one streaming Gemini call. Parser splits TRANSCRIPT/ANSWER inline. Saves ~1s per auto-answer. | ~4 h |
| **Unit tests for parsers** | Header-only test framework (doctest) wired into the build. Cover ExtractJsonStringField, DrainSSEBuffer, ColorizeBrackets, ParseSegments, BindingFromString. | ~1 d |
| **Refactor `OverlayWindow.cpp`** | ~1500 lines today. Split into Rendering, Hotkeys, SendPaths, Persistence. Mechanical but reduces regression surface for future features. | ~3 h |
| **Multiple conversations UI** | Today sessions exist as a single text field. Real picker would be a session-list panel + new/rename/delete buttons. | ~1 d |
| **Full syntax highlighting** | Per-language lexers (currently pan-language keyword set). Detect language from the code fence label and use the right keyword/operator set. | ~1 d |

---

## 6. Risks / open questions

- **API costs at scale.** If multiple users adopt and run auto mode all day, individual cost is fine (~$1/day heavy use). But that's per-user — you're not running the spend. Worth documenting expected cost in README.
- **Detection by interview tooling.** `WDA_EXCLUDEFROMCAPTURE` works against standard screen capture but newer "proctoring" tools may use lower-level APIs (mirror driver, DRM-protected paths). Untested.
- **Anthropic / OpenAI vision schemas** are implemented per their documented spec but **have not been tested with real API calls** (only Gemini has been end-to-end tested). May need debugging on first real use.
- **Custom (OpenAI-compatible) endpoint** path supports `http://` for local servers, but ParseUrl edge cases (IPv6, paths with query strings) are untested.
