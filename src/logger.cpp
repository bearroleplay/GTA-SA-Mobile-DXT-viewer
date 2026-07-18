#include "logger.h"
#include <ctime>

FILE*        Logger::s_file = nullptr;
std::wstring Logger::s_path;

bool Logger::Init(const std::wstring& exePath)
{
    std::wstring path = exePath;
    std::wstring::size_type dot = path.rfind(L'.');
    if (dot != std::wstring::npos)
        path = path.substr(0, dot);
    path += L".log";
    s_path = path;

    _wfopen_s(&s_file, path.c_str(), L"w");
    if (!s_file)
        return false;

    time_t t = time(nullptr);
    char tbuf[64];
    struct tm tm_info;
    localtime_s(&tm_info, &t);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm_info);

    fprintf(s_file, "=== DXTViewer log started %s ===\n", tbuf);
    fflush(s_file);
    return true;
}

void Logger::Shutdown()
{
    if (s_file)
    {
        fprintf(s_file, "=== DXTViewer log closed ===\n");
        fclose(s_file);
        s_file = nullptr;
    }
}

std::wstring Logger::GetLogPath()
{
    return s_path;
}

void Logger::Write(const char* level, const char* fmt, va_list args)
{
    if (!s_file)
        return;

    time_t t = time(nullptr);
    char tbuf[32];
    struct tm tm_info;
    localtime_s(&tm_info, &t);
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_info);

    fprintf(s_file, "[%s][%s] ", tbuf, level);
    vfprintf(s_file, fmt, args);
    fprintf(s_file, "\n");
    fflush(s_file);
}

void Logger::Debug(const char* file, int line, const char* fmt, ...)
{
    if (!s_file)
        return;
    const char* name = file;
    const char* slash = strrchr(file, '\\');
    if (!slash) slash = strrchr(file, '/');
    if (slash) name = slash + 1;

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    time_t t = time(nullptr);
    char tbuf[32];
    struct tm tm_info;
    localtime_s(&tm_info, &t);
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_info);

    fprintf(s_file, "[%s][DBG][%s:%d] %s\n", tbuf, name, line, buf);
    fflush(s_file);
}

void Logger::Info(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Write("INF", fmt, args);
    va_end(args);
}

void Logger::Warn(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Write("WRN", fmt, args);
    va_end(args);
}

void Logger::Error(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Write("ERR", fmt, args);
    va_end(args);
}
