# Invisible AI Overlay

The **Invisible AI Overlay** is a lightweight, undetectable Windows application that brings the power of Large Language Models (LLM) like Gemini and GPT directly to your screen without interrupting your workflow.

Designed for privacy and stealth, this overlay uses Windows native APIs to remain **invisible to screen capture software** (OBS, Discord, Snipping Tool, TeamViewer, etc.), making it perfect for private assistance during meetings, interviews, or presentations.

## Features

-   **👻 Completely Invisible to Screen Capture**: Uses `SetWindowDisplayAffinity` to exclude the window from all capture methods.
-   **🖱️ Click-Through & Transparent**: Interactions pass through to windows behind it until you activate it.
-   **🤖 Multi-Model Support**: Defaults to **Gemini 2.5 Flash Lite** (Fast & Free) with support for Pro and GPT models.
-   **🛠️ Interactive Setup**: First-run configuration dialog to select your model and enter your API key.
-   **🚀 Standalone Executable**: No external files required. Just run `overlay.exe` anywhere.
-   **⚡ Fast Chat**: Copy text + Hotkey to instantly send context to the AI.

---

## Installation

1.  Download `overlay.exe` from the [Releases Page](https://github.com/Zer0skillman/Interview-Hack).
2.  Place it in any folder you like.
3.  Run `overlay.exe`.

*Note: You may need to whitelist the application in your antivirus as custom overlays can sometimes be flagged.*

## First Time Setup

1.  On first launch, a **Configuration Dialog** will appear.
2.  **Select Model**: Choose your preferred model (e.g., Gemini 2.5 Flash Lite).
3.  **Enter API Key**: Paste your Google AI Studio or OpenAI API key.
    -   [Get a Free Gemini API Key here](https://aistudio.google.com/app/apikey)
4.  Click **Start Overlay**.

Your settings are saved to `llm_config.txt` in the same folder. To change them later, just edit that file or delete it to trigger the setup dialog again.

---

## Controls & Usage

| Hotkey | Action |
| :--- | :--- |
| **INS** (Insert) | **Send to AI**: Copies your current selection (or clipboard) and sends it to the AI. |
| **DEL** (Delete) | **Toggle Visibility**: Hides or Shows the overlay instantly. |
| **Page Up** | Scroll Chat History **Up**. |
| **Page Down** | Scroll Chat History **Down**. |
| **END** | **Exit** the application completely. |

### How to Chat
1.  **Select text** anywhere on your screen (browser, IDE, PDF, etc.).
2.  Press **Ctrl+C** to copy (or just Highlight if you have clipboard history enabled).
3.  Press **INS**.
4.  The overlay will display "Thinking..." and stream the answer in real-time.

---

## Build Instructions (For Developers)

If you want to modify the code or build from source:

### Requirements
-   MinGW-w64 (g++) OR Visual Studio Build Tools.
-   Windows SDK.

### Compilation Command (MinGW)
```powershell
g++ main.cpp OverlayWindow.cpp ConfigLoader.cpp LLMClient.cpp ConfigDialog.cpp -o overlay.exe -mwindows -static -DUNICODE -D_UNICODE -lwinhttp -lcomctl32
```

### File Structure
-   `main.cpp`: Entry point.
-   `OverLayWindow.cpp`: Core window logic & rendering.
-   `LLMClient.cpp`: HTTP client for Gemini/OpenAI APIs.
-   `ConfigDialog.cpp`: Initial setup UI.
-   `models_list.txt`: (Optional) External model list.

## License
MIT License
