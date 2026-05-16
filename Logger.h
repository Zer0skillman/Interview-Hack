#pragma once
#include <string>

// Tiny file-based logger. Lines go to logs/app.log with a wall-clock timestamp.
// Levels: I = info, W = warning, E = error. The logs/ directory is created on
// first use. Logging is best-effort — failures here are swallowed.
namespace Logger {
    void Log(char level, const std::string& msg);
    inline void Info(const std::string& m)  { Log('I', m); }
    inline void Warn(const std::string& m)  { Log('W', m); }
    inline void Error(const std::string& m) { Log('E', m); }
}
