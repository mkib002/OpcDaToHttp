/*
OpcDaToHttp

Параметры запуска приложения (разбор командной строки).
*/
#pragma once

#include <string>

#include "Logger.h"

struct AppConfig
{
    std::wstring host = L"localhost";         // "localhost"/"127.0.0.1"/"" -> локальный хост
    std::wstring serverProgId;                // пусто -> первый найденный локальный OPC DA сервер
    unsigned short port = 8080;               // 0 -> любой свободный порт
    unsigned long rateMs = 1000;              // период обновления OPC-группы, мс
    float deadBand = 0.0f;                    // зона нечувствительности группы
    unsigned long refreshSec = 0;             // 0 -> только подписка, без периодического refresh
    std::string itemsFile;                    // файл со списком тегов (по имени в строке)
    std::string filter;                       // подстрока для фильтра имён из namespace
    size_t maxItems = 0;                      // 0 -> без ограничения
    unsigned long reconnectSec = 10;          // 0 -> не переподключаться
    unsigned long maxConnections = 64;        // предел одновременных HTTP-соединений
    unsigned long staleMs = 0;                // 0 -> авто: 5 * rateMs, но не меньше 5000
    unsigned long requestTimeoutMs = 5000;    // ожидание refresh по ?refresh=1
    bool listServers = false;
    bool showHelp = false;
    LogLevel logLevel = LogLevel::Info;

    /// Порог, после которого значение считается устаревшим.
    /// По умолчанию: 10 периодов группы, но не меньше 60 с - для значений,
    /// которые сервер не пересылает (они не меняются), это норма, а не ошибка.
    unsigned long staleTimeoutMs() const
    {
        if (staleMs > 0)
        {
            return staleMs;
        }

        const unsigned long automatic = rateMs * 10;
        return automatic < 60000 ? 60000 : automatic;
    }

}; // AppConfig

bool ParseCommandLine(int argc, wchar_t *argv[], AppConfig &config, std::string &errorText);

std::string UsageText();
