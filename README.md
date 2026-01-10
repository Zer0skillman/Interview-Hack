# Invisible AI Overlay (Memory Enabled)

The **Invisible AI Overlay** is a lightweight, undetectable Windows application that brings the power of Linked Language Models (LLM) directly to your screen.

**New in v2.0:** Now features a **Local Python Backend** that gives the AI **Conversation Memory**! The AI remembers what you said previously in the session, allowing for real back-and-forth discussions.

## Architecture

The application consists of two components that run together:
1.  **`overlay.exe`** (Frontend): The invisible C++ window that handles drawing, hotkeys, and user input.
2.  **`ai_backend.exe`** (Backend): A hidden Python process that manages the AI logic and maintains conversation history.

*Note: You only need to launch `overlay.exe`. It automatically manages the backend for you.*

## Features

-   **🧠 Conversation Memory**: The AI remembers context from previous messages in the current session.
-   **👻 Completely Invisible**: Uses `SetWindowDisplayAffinity` to exclude the window from screen capture (OBS, Discord, etc.).
-   **🖱️ Click-Through**: Interact with windows behind the overlay seamlessly.
-   **🤖 Multi-Model Support**: Defaults to **Gemini** (fast & free) with OpenAI support structure.
-   **🚀 Standalone**: Zero-setup distribution. Everything is bundled into one executable package.

---

## Installation

1.  Download the **release zip**.
2.  Extract the `project_app` folder.
3.  Run `overlay.exe` inside it.

*Do not separate `overlay.exe` from `ai_backend.exe`. They must be in the same folder.*

## Setup

1.  On first run, you will be asked for your **API Key**.
    -   [Get a Free Gemini API Key here](https://aistudio.google.com/app/apikey)
2.  Select your model and click Start.
3.  The overlay will appear (invisible to others, visible to you).

---

## Controls & Usage

| Hotkey | Action |
| :--- | :--- |
| **INS** (Insert) | **Send to AI**: Copies selected text and sends it to the AI. |
| **DEL** (Delete) | **Toggle Visibility**: Hides or Shows the overlay. |
| **Page Up/Down** | Scroll Chat History. |
| **END** | **Exit** the application (closes both frontend and backend). |

### Memory Behavior
-   **Session Based**: The AI remembers everything you discuss while the app is open.
-   **Reset**: When you close the app (`END` key) or restart it, the memory is wiped for privacy.

---

## Build Instructions (Developers)

This project uses a **Master Build Script** to compile both the C++ Frontend and Python Backend.

**Requirements:**
1.  **Python 3.10+** (Added to PATH)
2.  **MinGW-w64** (g++) (Added to PATH)

**How to Build:**
Open PowerShell in the project root and run:
```powershell
.\build_release.ps1
```

This will:
1.  Install Python dependencies and build `ai_backend.exe` (using PyInstaller).
2.  Compile `overlay.exe` (using g++).
3.  Package everything into a ready-to-ship `project_app` folder.

## License
MIT License
