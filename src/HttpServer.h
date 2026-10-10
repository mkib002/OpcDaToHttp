/*
OpcDaToHttp

Минимальный HTTP/1.1 сервер на Winsock: без внешних зависимостей.
Обслуживает запросы в отдельных потоках, значения берёт из TagStore,
в OPC-поток обращается только для ?refresh=1.
*/
#pragma once

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "AppConfig.h"
#include "OpcDaClient.h"
#include "TagStore.h"

struct HttpRequest
{
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string version;
    std::map<std::string, std::string> headers; // ключи в нижнем регистре

}; // HttpRequest

struct HttpResponse
{
    int status = 200;
    std::string reason = "OK";
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
    bool closeConnection = false;
    std::vector<std::pair<std::string, std::string>> extraHeaders;

}; // HttpResponse

class HttpServer
{
  public:
    HttpServer(const AppConfig &config, TagStore &store, OpcDaClient &opc);

    ~HttpServer();

    /// Создать сокет, bind, listen и запустить поток приёма соединений
    bool start();

    /// Закрыть слушающий сокет и дождаться завершения обработчиков
    void stop();

    const std::string &lastError() const
    {
        return lastError_;
    }

    unsigned short boundPort() const
    {
        return boundPort_;
    }

  private:
    void acceptLoop();
    void handleConnection(SOCKET clientSocket);

    /// true - запрос прочитан (в earlyResponse либо 0, либо готовый ответ об ошибке)
    bool readRequest(SOCKET clientSocket, std::string &buffer, HttpRequest &request, HttpResponse &earlyResponse);

    HttpResponse route(const HttpRequest &request);

    bool sendResponse(SOCKET clientSocket, const HttpRequest &request, const HttpResponse &response, bool headOnly,
                      bool closeConnection);

    HttpResponse valueResponse(const std::string &name, bool forceRefresh);
    HttpResponse rawResponse(const std::string &name, bool forceRefresh);
    HttpResponse valuesResponse(const std::vector<std::string> &names, bool forceRefresh);
    HttpResponse itemsResponse();
    HttpResponse dumpResponse();
    HttpResponse healthResponse();
    HttpResponse helpResponse();
    void setLastError(const std::string &text);

    AppConfig config_;
    TagStore &store_;
    OpcDaClient &opc_;

    SOCKET listenSocket_ = INVALID_SOCKET;
    std::thread acceptThread_;
    std::atomic<bool> stopping_{false};
    std::atomic<size_t> activeConnections_{0};
    std::mutex connectionsMutex_;
    std::condition_variable connectionsCv_;
    std::string lastError_;
    unsigned short boundPort_ = 0;
    bool winsockStarted_ = false;
    ULONGLONG startTick_ = 0;

}; // HttpServer
