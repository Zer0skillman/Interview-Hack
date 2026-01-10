# Invisible Overlay Application

This application creates a top-most, transparent overlay that displays "HELLO" in green text. 
It uses the `SetWindowDisplayAffinity` API with `WDA_EXCLUDEFROMCAPTURE` to remain invisible to screen capture software like OBS, Discord, and Snipping Tool.

## Prerequisites

You need a C++ compiler. 
We attempted to install **MSYS2** automatically.
Once the installation finishes (you might see a terminal window):
1. Open **MSYS2 MINGW64** from your Start Menu.
2. Run this command to install GCC:
   ```bash
   pacman -S mingw-w64-x86_64-gcc
   ```
3. Add `C:\msys64\mingw64\bin` to your System PATH to use `g++` everywhere.

To check if you have it:
```powershell
g++ --version
```

If you prefer **Visual Studio Build Tools** (MSVC):
```powershell
winget install Microsoft.VisualStudio.2022.BuildTools --force --accept-package-agreements
```
*Note: This is a large installation.*

## Build Instructions

### Option 1: MinGW (g++) - Recommended for speed
Run this command in the terminal (this temporarily sets the path for you):
```powershell
$env:Path = "C:\msys64\mingw64\bin;" + $env:Path; g++ main.cpp OverlayWindow.cpp ConfigLoader.cpp LLMClient.cpp ConfigDialog.cpp -o overlay.exe -mwindows -static -DUNICODE -D_UNICODE -lwinhttp -lcomctl32
```
- `-lwinhttp`: Links the Windows HTTP library for networking.

### Option 2: Visual Studio (cl.exe)
Open the "Developer Command Prompt for VS 2022" and run:
```cmd
cl main.cpp OverlayWindow.cpp ConfigLoader.cpp LLMClient.cpp User32.lib Gdi32.lib Winhttp.lib /link /SUBSYSTEM:WINDOWS
```

## Running
1. **Configure API Key**: Open `llm_config.txt`.
   ```ini
   provider=gemini
   model=gemini-2.5-flash-lite
   api_key=INSERT_OR_CHANGE_KEY_HERE
   ```
   *Future support for `provider=openai` is built-in structure.*
2. Run `overlay.exe`.

## Controls
- **INS**: Copy selected text to clipboard and press **INS** to chat.
- **DEL**: Toggle Overlay Visibility (Hide/Show).
- **Page Up**: Scroll Chat History Up.
- **Page Down**: Scroll Chat History Down.
- **END**: Close the application.

## How to Use "Chat"
1. Copy any text to your clipboard (Ctrl+C).
2. Press the **INS** key on your keyboard.
3. The overlay will show "Thinking..." and then stream the AI response.

## Privacy Note
This application uses trusted Windows APIs (`SetWindowDisplayAffinity`). It does not inject code or modify other processes. It simply tells Windows "don't include this window in screen captures".
