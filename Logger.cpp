#define WIN32_LEAN_AND_MEAN
#include "Logger.h"
#include <windows.h>
#include <cstdio>
#include <mutex>

namespace Logger {

static std::mutex s_mutex;

void Log(char level, const std::string& msg) {
    std::lock_guard<std::mutex> lk(s_mutex);

    CreateDirectoryW(L"logs", NULL);
    HANDLE h = CreateFileW(L"logs\\app.log",
        FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    SetFilePointer(h, 0, NULL, FILE_END);
    SYSTEMTIME st; GetLocalTime(&st);

    char line[2048];
    int n = std::snprintf(line, sizeof(line),
        "%04d-%02d-%02d %02d:%02d:%02d %c %s\n",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond,
        level, msg.c_str());
    if (n > 0) {
        DWORD wrote = 0;
        WriteFile(h, line, (DWORD)n, &wrote, NULL);
    }
    CloseHandle(h);
}

} // namespace Logger
