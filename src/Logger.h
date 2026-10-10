/*
OpcDaToHttp

Потокобезопасный лог в stdout с меткой времени. Сообщения - в UTF-8.
*/
#pragma once

#include <string>

enum class LogLevel
{
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3

}; // LogLevel

void LogSetLevel(LogLevel level);

LogLevel LogGetLevel();

bool LogIsEnabled(LogLevel level);

const char *LogLevelName(LogLevel level);

/// Форматирование в стиле printf, результат - одна строка лога
void LogFormat(LogLevel level, const char *format, ...);

void LogWrite(LogLevel level, const std::string &message);

inline void LogDebug(const std::string &message)
{
    if (LogIsEnabled(LogLevel::Debug))
    {
        LogWrite(LogLevel::Debug, message);
    }
}

inline void LogInfo(const std::string &message)
{
    if (LogIsEnabled(LogLevel::Info))
    {
        LogWrite(LogLevel::Info, message);
    }
}

inline void LogWarn(const std::string &message)
{
    if (LogIsEnabled(LogLevel::Warn))
    {
        LogWrite(LogLevel::Warn, message);
    }
}

inline void LogError(const std::string &message)
{
    if (LogIsEnabled(LogLevel::Error))
    {
        LogWrite(LogLevel::Error, message);
    }
}

#define LOGF_DEBUG(...)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        if (LogIsEnabled(LogLevel::Debug))                                                                             \
        {                                                                                                              \
            LogFormat(LogLevel::Debug, __VA_ARGS__);                                                                   \
        }                                                                                                              \
    } while (0)

#define LOGF_INFO(...)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        if (LogIsEnabled(LogLevel::Info))                                                                              \
        {                                                                                                              \
            LogFormat(LogLevel::Info, __VA_ARGS__);                                                                    \
        }                                                                                                              \
    } while (0)

#define LOGF_WARN(...)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        if (LogIsEnabled(LogLevel::Warn))                                                                              \
        {                                                                                                              \
            LogFormat(LogLevel::Warn, __VA_ARGS__);                                                                    \
        }                                                                                                              \
    } while (0)

#define LOGF_ERROR(...)                                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        if (LogIsEnabled(LogLevel::Error))                                                                             \
        {                                                                                                              \
            LogFormat(LogLevel::Error, __VA_ARGS__);                                                                   \
        }                                                                                                              \
    } while (0)
