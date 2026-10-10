/*
OpcDaToHttp

Точка входа: разбор аргументов, запуск OPC-потока и HTTP-сервера,
корректная остановка по Ctrl+C.
*/
#include "HttpServer.h"

#include <windows.h>

#include <atomic>
#include <cstdio>

#include "AppConfig.h"
#include "Logger.h"
#include "OpcDaClient.h"
#include "StringUtil.h"
#include "TagStore.h"

namespace
{

std::atomic<bool> g_stopRequested{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD eventType)
{
    switch (eventType)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stopRequested.store(true);
        return TRUE;
    default:
        return FALSE;
    }

}

}

int wmain(int argc, wchar_t *argv[])
{
    setlocale(LC_ALL, "");

    AppConfig config;
    std::string parseError;

    if (!ParseCommandLine(argc, argv, config, parseError))
    {
        std::printf("%s\n\n%s", parseError.c_str(), UsageText().c_str());
        return 2;
    }

    if (config.showHelp)
    {
        std::printf("%s", UsageText().c_str());
        return 0;
    }

    LogSetLevel(config.logLevel);
    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    TagStore store;
    OpcDaClient opc(config, store);

    opc.start();

    if (config.listServers)
    {
        opc.waitUntilReady(15000);
        opc.stop();
        return 0;
    }

    HttpServer http(config, store, opc);
    if (!http.start())
    {
        LOGF_ERROR("HTTP: не удалось запустить сервер: %s", http.lastError().c_str());
        opc.stop();
        return 1;
    }

    LOGF_INFO("HTTP: сервер слушает на всех сетевых интерфейсах (0.0.0.0), порт %u",
              static_cast<unsigned>(http.boundPort()));
    LOGF_INFO("HTTP: локально доступен: http://127.0.0.1:%u/", static_cast<unsigned>(http.boundPort()));

    if (!opc.waitUntilReady(20000) && !opc.isConnected())
    {
        LOGF_WARN("OPC: подключение не подтверждено (%s); состояние - в GET /health", opc.lastError().c_str());
    }

    while (!g_stopRequested.load())
    {
        ::Sleep(200);
    }

    LogInfo("Получен сигнал остановки, завершаем работу...");

    http.stop();
    opc.stop();

    LogInfo("Остановлено.");
    return 0;

}
