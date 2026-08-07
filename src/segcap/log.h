// log.h -- logging for code running inside someone else's process.
//
// Constraints that shape this:
//   - The host may have no console, so stdout is not a reliable channel.
//   - We are called from the render thread; logging must not allocate on a hot
//     path or take a contended lock for long.
//   - If we crash the game, the log is the only evidence of what we were doing,
//     so it must be flushed eagerly rather than buffered.
//
// Output goes to a file next to the DLL and to OutputDebugString, so it is
// visible in DebugView / a debugger even if the file cannot be created.

#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace segcap {

class Log {
public:
    static Log& Get() {
        static Log instance;
        return instance;
    }

    void Init(HMODULE self) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_) return;

        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameW(self, path, MAX_PATH)) {
            std::wstring p(path);
            const size_t dot = p.find_last_of(L'.');
            if (dot != std::wstring::npos) p = p.substr(0, dot);
            p += L".log";
            _wfopen_s(&file_, p.c_str(), L"w");
        }
        start_ = GetTickCount64();
    }

    void Write(const char* level, const char* fmt, va_list ap) {
        char body[1024];
        _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);

        char line[1200];
        const unsigned long long ms = GetTickCount64() - start_;
        _snprintf_s(line, sizeof(line), _TRUNCATE, "[segcap %6llu.%03llu %-5s] %s\n",
                    ms / 1000, ms % 1000, level, body);

        std::lock_guard<std::mutex> lock(mutex_);
        OutputDebugStringA(line);
        if (file_) {
            std::fputs(line, file_);
            // Flushed every line on purpose: if we take the game down, the last
            // line written is the most valuable one in the file.
            std::fflush(file_);
        }
    }

private:
    Log() = default;
    ~Log() {
        if (file_) std::fclose(file_);
    }

    std::mutex mutex_;
    std::FILE* file_ = nullptr;
    unsigned long long start_ = 0;
};

inline void LogInfo(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Log::Get().Write("info", fmt, ap);
    va_end(ap);
}

inline void LogWarn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Log::Get().Write("warn", fmt, ap);
    va_end(ap);
}

inline void LogError(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Log::Get().Write("ERROR", fmt, ap);
    va_end(ap);
}

}  // namespace segcap
