#include "Logger.h"
#include <cstdio>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <mutex>

namespace Logger {

static std::mutex s_mutex;

void Log(char level, const std::string& msg) {
    std::lock_guard<std::mutex> lk(s_mutex);

    std::error_code ec;
    std::filesystem::create_directory("logs", ec);

    std::ofstream f("logs/app.log", std::ios::app | std::ios::binary);
    if (!f.is_open()) return;

    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif

    char line[2048];
    int n = std::snprintf(line, sizeof(line),
        "%04d-%02d-%02d %02d:%02d:%02d %c %s\n",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
        level, msg.c_str());
    if (n > 0) f.write(line, n);
}

} // namespace Logger
