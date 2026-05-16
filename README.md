# Invisible AI Overlay (Memory Enabled)

The **Invisible AI Overlay** is a lightweight, undetectable Windows application that brings the power of Large Language Models (LLMs) directly to your screen.

**v2.1:** Single-process C++ build. Conversation history is kept in-memory in `overlay.exe` itself and sent on each request, so the AI remembers context across the session without a separate backend process.

## Architecture

A single executable, `overlay.exe`, written in C++:
-   Hosts the invisible Win32 window, hotkeys, and chat rendering.
-   Talks directly to the Gemini API over HTTPS (`generativelanguage.googleapis.com`), with streaming responses.
-   Captures system audio via WASAPI loopback (a rolling 30s ring buffer) so F8 can include what the interviewer just said. Your microphone is **not** captured.
-   Captures screenshots via GDI for screen-share interview questions.
-   Holds the conversation history in memory and replays it on every call.

## Features

-   **🧠 Conversation Memory**: The AI remembers context from previous messages in the current session.
-   **👻 Completely Invisible**: Uses `SetWindowDisplayAffinity` to exclude the window from screen capture (OBS, Discord, etc.).
-   **🖱️ Click-Through**: Interact with windows behind the overlay seamlessly.
-   **🤖 Gemini Support**: Defaults to **Gemini** (fast & free). Multi-provider support is on the roadmap.
-   **🚀 Standalone**: Single `.exe`, no separate backend, no Python runtime needed.

---

## Installation

1.  Download the **release zip**.
2.  Extract the `project_app` folder.
3.  Run `overlay.exe` inside it.

## Setup

1.  On first run, you will be asked for your **API Key**.
    -   [Get a Free Gemini API Key here](https://aistudio.google.com/app/apikey)
2.  Select your model and click Start.
3.  The overlay will appear (invisible to others, visible to you).

---

## Controls & Usage

| Hotkey | Action |
| :--- | :--- |
| **INS** (Insert) | **Send clipboard text**: Sends whatever is currently in your clipboard to the AI. |
| **F8** | **Capture screen + audio + ask**: Screenshots the monitor under your cursor *and* attaches the last 30 seconds of meeting audio (WASAPI loopback — system audio only, not your mic). Gemini transcribes the audio internally and answers the question. Use this when the interviewer is speaking. Audio is silently dropped for non-Gemini providers. |
| **F9** | **Toggle auto-answer mode** (Gemini only): when ON, the overlay continuously listens; every 3-8s (adaptive based on detected speech) it sends recent audio to Gemini with a strict classifier. If a substantive technical question is detected, a streaming answer is added to the chat automatically — no key needed. The transcript bar at the bottom shows what was heard. Voice activity gates the poll so silent rooms don't burn API calls. Cost: ~$0.10-0.20/hr on Gemini Flash. |
| **F10** | **Move/Resize mode**: temporarily disables click-through so you can drag the overlay around or resize from its edges. Press F10 again to lock it back in place. A red banner appears at the top while active. |
| **Ctrl+R** | **Reset conversation**: clears all chat history immediately. |
| **Ctrl+C** | **Copy last AI answer**: copies the most recent AI response to your clipboard. |
| **DEL** (Delete) | **Toggle Visibility**: Hides or shows the overlay. |
| **Page Up/Down** | Scroll chat history. |
| **END** | **Exit** the application. |

### Memory Behavior
-   **Session Based**: The AI remembers everything you discuss while the app is open.
-   **Reset**: When you close the app (`END` key) or restart it, the memory is wiped for privacy.

---

## Build Instructions (Developers)

**Requirements:**
1.  **MinGW-w64** (g++) (added to PATH)

**How to Build:**
Open PowerShell in the project root and run:
```powershell
.\build_release.ps1
```

This will:
1.  Compile `overlay.exe` (using g++ with WinHTTP).
2.  Package it plus `models_list.txt` and `README.md` into a ready-to-ship `project_app` folder.

## License
MIT License
