# Invisible AI Overlay — Project Status & Roadmap

**Last updated:** 2026-05-16
**Current version:** 2.2.0
**Status:** Critical + HIGH + most of NORMAL complete. The product is well past v1.0 quality. Remaining work is the genuinely-large NORMAL items (multi-session, full markdown, full syntax highlighting, edit/regenerate) plus LOW polish.

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
| `F11` | **Open runtime settings** (re-show welcome) | fixed |
| `Ctrl+R` | Reset conversation | ✅ |
| `Ctrl+C` | Copy last AI answer | ✅ |
| `Ctrl+Shift+C` | Select-mode: click any chat bubble to copy it | ✅ |
| `Ctrl+E` | **Export chat to markdown** (opens the file) | fixed |
| `Ctrl+F` | **Open chat search bar** (type to filter, Enter to jump) | fixed |
| `Ctrl+=` / `Ctrl+-` | **Grow / shrink chat font** (persisted) | fixed |
| `F1` | **About dialog** | fixed |
| `F11` | Open runtime settings | fixed |
| `Shift+←` / `Shift+→` | Scroll code blocks horizontally | fixed |
| `INS` | Send clipboard text | fixed |
| `DEL` | Hide/show overlay | fixed |
| `PgUp` / `PgDn` | Scroll chat | fixed |
| `END` | Exit | fixed |

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
- ✅ **Conversation persists across runs** (`chat.txt`, gated by `restore_session` config). Saved on exit, loaded on launch. Wiped by Ctrl+R.
- ✅ **Markdown export** via `Ctrl+E` → `chat_export_YYYYMMDD_HHMM.md`, auto-opens in default markdown viewer.
- ✅ **Chat search**: `Ctrl+F` opens a search bar at the top; type to filter, Enter scrolls to first match, Esc closes.

### Appearance & personalization
- ✅ **3 built-in themes** (`dark` default, `light`, `contrast`) — all colors driven by a `ThemeColors` struct
- ✅ **Adjustable opacity** (60-255 alpha, persisted as `opacity_alpha`)
- ✅ **Adjustable font size** for prose and code (Ctrl+= / Ctrl+- to grow/shrink, persisted)
- ✅ **Optional per-message timestamps** (`show_timestamps` config) — dim HH:MM in top-right corner of bubble
- ✅ **Code block language label** — language identifier after the opening ``` (e.g. `python`) rendered as a small accent-colored tag in the top-right of the code block
- ✅ **Token meter** in the transcript bar — approximate cumulative input/output tokens this session
- ✅ **About dialog** (F1) — version, license, brief description

### Distribution
- ✅ **Embedded application icon** (32×32 PNG-in-ICO, "AI" mark on blue)
- ✅ **Version + product metadata** in `overlay.exe` (FileVersion 2.1.0.0, ProductName, FileDescription, LegalCopyright) — visible in Windows file properties

---

## 4. Behavioral quirks worth knowing (not bugs, but call them out)

- **Polling fires when F9 is OFF** (just for the transcript bar). Costs ~$0.10/hr on Gemini Flash. Intentional but wasteful — see `[Critical] Gate polling on F9`.
- **F8 is screenshot-only now.** Combined screen+audio is only done via auto mode (F9) — by design after the F8/F7 split.
- **Audio dropped silently for non-Gemini providers.** Only Gemini accepts raw audio. F7 will show a chat message if you press it on Claude/OpenAI, but auto-mode polls just no-op.
- **Loopback follows the default audio device.** If you unplug headphones mid-session and Windows switches output, the capture thread may need a restart. Untested.
- **Conversation history grows unbounded** in memory until app exit or `Ctrl+R`.

---

## 5. Roadmap

Priorities are about **product completeness for shipping to non-developer users**, not technical interest.

---

### 🔴 CRITICAL — _all clear_ ✅

All seven critical items completed in 2.1.0. See **Reliability**, **UI**, **Distribution** above for what landed. The user must still **rotate the historically-leaked API key** at https://aistudio.google.com/app/apikey — that's a manual action no code can do for them. The welcome screen now warns if the leaked key is in use, and the "Clear" button makes it one click to remove.

---

### 🟠 HIGH — _all clear_ ✅

All 14 HIGH items complete. Notable additions in the final wave:
- **Audio device picker**: separate combos for output (loopback) and microphone in the welcome screen. AudioCapture enumerates active endpoints via `IMMDeviceEnumerator::EnumAudioEndpoints`, persists chosen IDs in `llm_config.txt`, and falls back to default if a saved device is no longer present.
- **Per-message copy**: new `Ctrl+Shift+C` enters select mode (blue banner). Click any bubble to copy its text — strips `[tag]` prefixes from user bubbles, shows "Copied to clipboard" in the transcript bar, auto-exits. Esc cancels.
- Click-through is now properly tri-state (off / move-mode / select-mode) with a single `UpdateClickThrough()` helper that flips `WS_EX_TRANSPARENT` based on the active mode.

---

### 🟡 NORMAL — 11 / 18 complete

The 11 completed items are listed in the Feature inventory above (Sessions & export, Appearance & personalization, Reliability). Seven remain, each with honest deferral rationale:

| Item | Why deferred | Effort |
|---|---|---|
| **Multiple conversations / named sessions** | Significant new UI: session list, switcher, per-session chat files, naming. Real product feature, not polish. Better as its own focused wave. | 1 d |
| **Edit / regenerate question** | Needs in-place text editing inside a bubble (no Win32 control hosting in current renderer) plus history-slicing logic to drop the old reply and re-fire. Touches rendering + state in non-trivial ways. | 3 h |
| **Full syntax highlighting beyond brackets** | Real per-language token highlighting (keywords, strings, comments) needs at least minimal per-language lexers. Bracket pair colorization already solves the worst readability issue (nesting confusion). Diminishing returns vs. effort. | 1 d |
| **Markdown rendering beyond code blocks** | Tables, headings, bullets, bold/italic. Requires a real markdown parser + significant OnPaint rework. Current `**bold**` etc. shows as literal markdown — works but unpolished. Defer until users complain. | 1–2 d |
| **Hotkey rebinding for system keys** (INS, DEL, END, PgUp/Dn) | Rebindable hotkeys already cover the 7 semantic actions. Repurposing standard window keys (especially DEL = hide and END = exit) risks accidental data loss if users bind something destructive. Low value vs. risk. | 2 h |
| **Sound effect on auto-answer** | Default-on would be obnoxious; default-off makes it discoverable only via a setting most users won't find. Better solved by the visible in-flight indicator we already have. | 1 h |
| **Streaming auto-answer (single-call merge)** | Currently 2 calls (classifier + answer). Merging into 1 call with structured output is more complex and ~1 second faster in best case. The 2-call architecture is clearer to debug. Net win unclear. | 4 h |

If you ship v1 and these become real pain points based on usage, that's a strong signal to revisit.

---

### 🟢 LOW — polish

| Item | Why it matters | Effort |
|---|---|---|
| **Auto-updater** | Check GitHub Releases on launch, download new builds. | 1 d |
| **MSI installer** instead of zip | Optional but more "real product" feel. | 1 d |
| **Tooltip help on hover** in welcome screen | Explain each field. | 1 h |
| **Visual hotkey hints overlay** | Press-and-hold a key to flash a cheat sheet on screen. | 2 h |
| **Cross-platform** (macOS / Linux) | Major rewrite — WASAPI, Win32, GDI+ all replaced. Whole separate project really. | 1+ months | - Not Now Skip this
| **CONTRIBUTING.md, code of conduct** | If open-sourcing seriously. | 1 h |
| **README screenshots / demo gif** | Make the GitHub page actually look like something. | 1 h |
| **FAQ section in README** | Common questions: "is it detectable", "what data is sent", "cost per hour". | 1 h |
| **Build CI** | GitHub Actions to build on push, attach binary to release. | 2 h |
| **Unit tests for parsers** (JSON extract, SSE, hotkey serialization) | These are easy-to-test pure functions and have already had subtle bugs. | 1 d |
| **Logging system** (structured, level-based, file output) | Better than `OutputDebugString` for diagnosing user-reported issues. | 3 h |
| **Refactor `OverlayWindow.cpp` into smaller files** | It's >1000 lines now. Split rendering, hotkeys, audio glue, send paths. | 3 h |
| **Extract magic numbers** to constants (timer IDs, hotkey IDs, font sizes, etc.) | Cleanup. | 1 h |
| **Use string resources** instead of inline `L"..."` literals | Easier translation later. | 2 h |

---

### 🚫 Explicitly out of scope (deferred or rejected)

| Item | Why not |
|---|---|
| **Mobile / phone version** | Not the use case. Interviews happen on laptops. |
| **In-overlay video capture** | Same use case is solved by screenshot + audio. |
| **Local LLM via Whisper-only path** | We compared and Gemini multimodal beats local Whisper for quality and is far simpler. Local LLM is supported via the "Custom (OpenAI-compatible)" provider for Ollama users who want it. |
| **GUI for raw config file editing** | `llm_config.txt` is small and self-explanatory; the welcome screen covers all of it. |
| **Cloud-synced settings** | Single-user tool. No backend. |
| **Plugin system** | Way too early. |

---

## 6. Suggested order if you were going for v1.0 release

Critical: done. HIGH: done. NORMAL: 11/18 done. The product is **past v1.0 quality** and is genuinely ready for the user's own use today. For public release, suggested path:

1. **Real-world smoke test** in a mock interview — concrete feedback for the 7 deferred NORMAL items
2. **README screenshots + GIF** (~1 h) — biggest legitimacy boost
3. **Pick 1-2 deferred NORMAL items based on smoke-test feedback** — most likely candidates: multi-session, edit/regenerate, or markdown
4. **LOW polish pass** (code signing, installer, auto-update) only if planning to distribute widely
5. **Tag v2.2.0 → v1.0 marketing version**

Everything in the LOW bucket is genuinely optional polish. You're done with the must-haves.

---

## 7. Risks / open questions

- **API costs at scale.** If multiple users adopt and run auto mode all day, individual cost is fine (~$1/day heavy use). But that's per-user — you're not running the spend. Worth documenting expected cost in README.
- **Detection by interview tooling.** `WDA_EXCLUDEFROMCAPTURE` works against standard screen capture but newer "proctoring" tools may use lower-level APIs (mirror driver, DRM-protected paths). Untested.
- **Anthropic / OpenAI vision schemas** are implemented per their documented spec but **have not been tested with real API calls** (only Gemini has been end-to-end tested). May need debugging on first real use.
- **Custom (OpenAI-compatible) endpoint** path supports `http://` for local servers, but ParseUrl edge cases (IPv6, paths with query strings) are untested.
