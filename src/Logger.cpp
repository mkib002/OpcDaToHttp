#include "Logger.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace
{

std::mutex g_logMutex;
std::atomic<int> g_logLevel(static_cast<int>(LogLevel::Info));
bool g_consoleConfigured = false;

void EnsureConsoleUtf8()
{
    if (!g_consoleConfigured)
    {
        g_consoleConfigured = true;
        ::SetConsoleOutputCP(CP_UTF8);
    }

}

const char *LevelTag(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Debug:
        return "DBG";
    case LogLevel::Info:
        return "INF";
    case LogLevel::Warn:
        return "WRN";
    case LogLevel::Error:
        return "ERR";
    default:
        return "???";
    }

}

}

void LogSetLevel(LogLevel level)
{
    g_logLevel.store(static_cast<int>(level), std::memory_order_relaxed);

}

LogLevel LogGetLevel()
{
    return static_cast<LogLevel>(g_logLevel.load(std::memory_order_relaxed));

}

bool LogIsEnabled(LogLevel level)
{
    return static_cast<int>(level) >= g_logLevel.load(std::memory_order_relaxed);

}

const char *LogLevelName(LogLevel level)
{
    return LevelTag(level);

}

void LogWrite(LogLevel level, const std::string &message)
{
    if (!LogIsEnabled(level))
    {
        return;
    }

    EnsureConsoleUtf8();

    SYSTEMTIME now = {0};
    ::GetLocalTime(&now);

    std::lock_guard<std::mutex> lock(g_logMutex);
    std::printf("%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] %s\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                now.wSecond, now.wMilliseconds, LevelTag(level), message.c_str());
    std::fflush(stdout);

}

void LogFormat(LogLevel level, const char *format, ...)
{
    if (!LogIsEnabled(level))
    {
        return;
    }

    char stackBuffer[1024];

    va_list args;
    va_start(args, format);
    const int written = _vsnprintf_s(stackBuffer, sizeof(stackBuffer), _TRUNCATE, format, args);
    va_end(args);

    if (written >= 0)
    {
        LogWrite(level, std::string(stackBuffer, static_cast<size_t>(written)));
        return;
    }

    LogWrite(level, std::string(stackBuffer));

}
