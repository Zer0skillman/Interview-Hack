#pragma once

// Tiny compatibility shim for non-Windows builds. The cross-platform code
// (ConfigLoader, HotkeyBinding, theme colors) was originally written against
// <windows.h> and uses MOD_*, VK_*, RGB(), UINT freely. Rather than refactor
// every call site, we define those symbols when not on Windows so the same
// .cpp/.h files compile on macOS.
//
// Usage:
//   #ifdef _WIN32
//   #include <windows.h>
//   #else
//   #include "WinCompat.h"
//   #endif

#ifndef _WIN32

#include <cstdint>

// RegisterHotKey modifier bits. The values are arbitrary on Mac — they're just
// flags used inside our HotkeyBinding struct and translated to Carbon at
// register time. Match the Win32 values so saved bindings parse identically
// across platforms.
#ifndef MOD_ALT
#define MOD_ALT      0x0001
#define MOD_CONTROL  0x0002
#define MOD_SHIFT    0x0004
#define MOD_WIN      0x0008
#endif

// Virtual-key codes. Same trick — the values mirror Win32 so the saved
// llm_config.txt is identical across platforms. The Mac hotkey impl
// translates these to Carbon kVK_* at register time.
#ifndef VK_F1
#define VK_F1     0x70
#define VK_F2     0x71
#define VK_F3     0x72
#define VK_F4     0x73
#define VK_F5     0x74
#define VK_F6     0x75
#define VK_F7     0x76
#define VK_F8     0x77
#define VK_F9     0x78
#define VK_F10    0x79
#define VK_F11    0x7A
#define VK_F12    0x7B
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_HOME   0x24
#define VK_END    0x23
#define VK_PRIOR  0x21   // PageUp
#define VK_NEXT   0x22   // PageDown
#define VK_SPACE  0x20
#define VK_TAB    0x09
#define VK_RETURN 0x0D
#define VK_BACK   0x08
#define VK_ESCAPE 0x1B
#endif

typedef unsigned int UINT;
typedef unsigned long DWORD;

// COLORREF is 0x00BBGGRR (Win32 endian). Keep the same packing so the rest of
// the code (which uses (color >> 0) & 0xFF for red) works unchanged.
typedef unsigned long COLORREF;
#ifndef RGB
#define RGB(r,g,b) ((COLORREF)( ((unsigned)(unsigned char)(r))        \
                              | ((unsigned)(unsigned char)(g) << 8)   \
                              | ((unsigned)(unsigned char)(b) << 16) ))
#define GetRValue(c) ((unsigned char)((c)        & 0xFF))
#define GetGValue(c) ((unsigned char)(((c) >> 8) & 0xFF))
#define GetBValue(c) ((unsigned char)(((c) >> 16)& 0xFF))
#endif

#endif // !_WIN32
