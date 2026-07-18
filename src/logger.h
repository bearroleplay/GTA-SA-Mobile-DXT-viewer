#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>

// To disable debug output: comment out the LOG_DEBUG calls in source,
// or comment the line below to strip all debug logging at compile time.
#define ENABLE_DEBUG_LOG

#ifdef ENABLE_DEBUG_LOG
#define LOG_DEBUG(fmt, ...) Logger::Debug(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif

#define LOG_INFO(fmt, ...)  Logger::Info(fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  Logger::Warn(fmt, ##__VA_ARGS__)
#define LOG_ERR(fmt, ...)   Logger::Error(fmt, ##__VA_ARGS__)

class Logger
{
public:
    static bool     Init(const std::wstring& exePath);
    static void     Shutdown();
    static void     Debug(const char* file, int line, const char* fmt, ...);
    static void     Info(const char* fmt, ...);
    static void     Warn(const char* fmt, ...);
    static void     Error(const char* fmt, ...);
    static std::wstring GetLogPath();

private:
    static void     Write(const char* level, const char* fmt, va_list args);
    static FILE*    s_file;
    static std::wstring s_path;
};
