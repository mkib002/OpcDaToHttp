#include "HttpServer.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "Logger.h"
#include "OpcValueFormatter.h"
#include "StringUtil.h"

namespace
{

const size_t kMaxHeaderBytes = 32 * 1024;      // предел заголовков запроса
const int kMaxRequestsPerConnection = 100;     // keep-alive ограничен, чтобы не копить мёртвые соединения
const ULONGLONG kConnectionLifetimeMs = 60000; // и по времени
const int kReceiveTimeoutMs = 5000;
const int kSendTimeoutMs = 10000;

std::string StatusReason(int status)
{
    switch (status)
    {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 413:
        return "Payload Too Large";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    default:
        return "Unknown";
    }

}

HttpResponse MakeTextResponse(int status, const std::string &body)
{
    HttpResponse response;
    response.status = status;
    response.reason = StatusReason(status);
    response.contentType = "text/plain; charset=utf-8";
    response.body = body;
    return response;

}

HttpResponse MakeJsonResponse(int status, const std::string &json)
{
    HttpResponse response;
    response.status = status;
    response.reason = StatusReason(status);
    response.contentType = "application/json; charset=utf-8";
    response.body = json;
    return response;

}

HttpResponse MakeErrorResponse(int status, const std::string &message, const std::string &details = std::string())
{
    std::string json = "{\"error\":\"" + strutil::JsonEscape(message) + "\"";
    if (!details.empty())
    {
        json += ",\"details\":\"" + strutil::JsonEscape(details) + "\"";
    }

    json += ",\"status\":" + strutil::Format("%d", status) + "}";

    return MakeJsonResponse(status, json);

}

std::string Boolean(bool value)
{
    return value ? "true" : "false";

}

bool IsTruthy(const std::string &value)
{
    return value == "1" || strutil::EqualsIgnoreCaseAscii(value, "true") || strutil::EqualsIgnoreCaseAscii(value, "yes");

}

std::map<std::string, std::vector<std::string>> ParseQuery(const std::string &query)
{
    std::map<std::string, std::vector<std::string>> result;

    const std::vector<std::string> pairs = strutil::Split(query, '&');
    for (size_t i = 0; i < pairs.size(); ++i)
    {
        if (pairs[i].empty())
        {
            continue;
        }

        const size_t separator = pairs[i].find('=');
        std::string key;
        std::string value;
        if (separator == std::string::npos)
        {
            key = pairs[i];
        }
        else
        {
            key = pairs[i].substr(0, separator);
            value = pairs[i].substr(separator + 1);
        }

        std::string decodedKey;
        std::string decodedValue;
        strutil::UrlDecode(key, decodedKey);
        strutil::UrlDecode(value, decodedValue);

        result[decodedKey].push_back(decodedValue);
    }

    return result;

}

}

HttpServer::HttpServer(const AppConfig &config, TagStore &store, OpcDaClient &opc)
    : config_(config), store_(store), opc_(opc)
{
    startTick_ = ::GetTickCount64();

}

HttpServer::~HttpServer()
{
    stop();

    if (winsockStarted_)
    {
        ::WSACleanup();
        winsockStarted_ = false;
    }

}

void HttpServer::setLastError(const std::string &text)
{
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    lastError_ = text;

}

bool HttpServer::start()
{
    WSADATA wsaData = {0};
    if (::WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        setLastError("WSAStartup не удался");
        return false;
    }

    winsockStarted_ = true;

    listenSocket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket_ == INVALID_SOCKET)
    {
        setLastError(strutil::Format("socket() не удался, код %d", ::WSAGetLastError()));
        return false;
    }

    BOOL reuseAddress = TRUE;
    ::setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuseAddress),
                 sizeof(reuseAddress));

    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(config_.port);

    if (::bind(listenSocket_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        setLastError(strutil::Format("bind() на порт %u не удался, код %d", static_cast<unsigned>(config_.port),
                                     ::WSAGetLastError()));
        ::closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    if (::listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR)
    {
        setLastError(strutil::Format("listen() не удался, код %d", ::WSAGetLastError()));
        ::closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    sockaddr_in boundAddress;
    int boundAddressSize = sizeof(boundAddress);
    std::memset(&boundAddress, 0, sizeof(boundAddress));
    if (::getsockname(listenSocket_, reinterpret_cast<sockaddr *>(&boundAddress), &boundAddressSize) == 0)
    {
        boundPort_ = ntohs(boundAddress.sin_port);
    }
    else
    {
        boundPort_ = config_.port;
    }

    stopping_.store(false);
    acceptThread_ = std::thread(&HttpServer::acceptLoop, this);
    return true;

}

void HttpServer::stop()
{
    stopping_.store(true);

    if (listenSocket_ != INVALID_SOCKET)
    {
        ::shutdown(listenSocket_, SD_BOTH);
        ::closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }

    if (acceptThread_.joinable())
    {
        acceptThread_.join();
    }

    // ждём завершения обработчиков соединений (у них свой таймаут recv/send)
    const ULONGLONG deadline = ::GetTickCount64() + 10000;
    std::unique_lock<std::mutex> lock(connectionsMutex_);
    while (activeConnections_.load() > 0 && ::GetTickCount64() < deadline)
    {
        connectionsCv_.wait_for(lock, std::chrono::milliseconds(100));
    }

    if (activeConnections_.load() > 0)
    {
        LOGF_WARN("HTTP: не дождались завершения соединений: %zu", activeConnections_.load());
    }

}

void HttpServer::acceptLoop()
{
    const SOCKET listening = listenSocket_;

    while (!stopping_.load())
    {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(listening, &readSet);

        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000; // 200 мс - чтобы регулярно проверять stopping_

        const int ready = ::select(0, &readSet, nullptr, nullptr, &timeout);
        if (ready <= 0)
        {
            continue;
        }

        sockaddr_in clientAddress;
        int clientAddressSize = sizeof(clientAddress);
        std::memset(&clientAddress, 0, sizeof(clientAddress));
        const SOCKET clientSocket =
            ::accept(listening, reinterpret_cast<sockaddr *>(&clientAddress), &clientAddressSize);
        if (clientSocket == INVALID_SOCKET)
        {
            if (stopping_.load())
            {
                break;
            }

            LOGF_WARN("HTTP: accept() не удался, код %d", ::WSAGetLastError());
            continue;
        }

        if (activeConnections_.load() >= config_.maxConnections)
        {
            const std::string body = "{\"error\":\"too many connections\"}";
            std::string response = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json; charset=utf-8\r\n";
            response += strutil::Format("Content-Length: %zu\r\nConnection: close\r\n\r\n", body.size());
            response += body;
            ::send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
            ::shutdown(clientSocket, SD_BOTH);
            ::closesocket(clientSocket);
            LogWarn("HTTP: превышен предел одновременных соединений, запрос отклонён");
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            activeConnections_.fetch_add(1);
        }

        std::thread(&HttpServer::handleConnection, this, clientSocket).detach();
    }

    LogInfo("HTTP: поток приёма соединений завершён");

}

void HttpServer::handleConnection(SOCKET clientSocket)
{
    const int receiveTimeout = kReceiveTimeoutMs;
    const int sendTimeout = kSendTimeoutMs;
    ::setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&receiveTimeout),
                 sizeof(receiveTimeout));
    ::setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&sendTimeout),
                 sizeof(sendTimeout));

    std::string buffer;
    int servedRequests = 0;
    const ULONGLONG connectionDeadline = ::GetTickCount64() + kConnectionLifetimeMs;

    while (!stopping_.load())
    {
        HttpRequest request;
        HttpResponse earlyResponse;

        if (!readRequest(clientSocket, buffer, request, earlyResponse))
        {
            break;
        }

        HttpResponse response = earlyResponse.status != 200 ? earlyResponse : route(request);

        bool close = response.closeConnection;
        std::map<std::string, std::string>::const_iterator connectionHeader = request.headers.find("connection");
        const bool clientKeepAlive =
            connectionHeader != request.headers.end() &&
            strutil::ContainsIgnoreCaseAscii(connectionHeader->second, "keep-alive");
        const bool clientWantsClose =
            connectionHeader != request.headers.end() && strutil::ContainsIgnoreCaseAscii(connectionHeader->second, "close");

        if (request.version == "HTTP/1.0")
        {
            if (!clientKeepAlive)
            {
                close = true;
            }
        }
        else if (clientWantsClose)
        {
            close = true;
        }

        const bool headOnly = request.method == "HEAD";
        if (!sendResponse(clientSocket, request, response, headOnly, close))
        {
            break;
        }

        ++servedRequests;
        if (close || servedRequests >= kMaxRequestsPerConnection || ::GetTickCount64() > connectionDeadline)
        {
            break;
        }
    }

    ::shutdown(clientSocket, SD_BOTH);
    ::closesocket(clientSocket);

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        activeConnections_.fetch_sub(1);
    }

    connectionsCv_.notify_all();

}

bool HttpServer::readRequest(SOCKET clientSocket, std::string &buffer, HttpRequest &request, HttpResponse &earlyResponse)
{
    size_t headerEnd = buffer.find("\r\n\r\n");
    while (headerEnd == std::string::npos)
    {
        char chunk[4096];
        const int received = ::recv(clientSocket, chunk, sizeof(chunk), 0);
        if (received <= 0)
        {
            return false;
        }

        buffer.append(chunk, static_cast<size_t>(received));
        if (buffer.size() > kMaxHeaderBytes)
        {
            earlyResponse = MakeErrorResponse(431, "request header too large");
            return true;
        }

        headerEnd = buffer.find("\r\n\r\n");
    }

    const std::string headerBlock = buffer.substr(0, headerEnd);
    buffer.erase(0, headerEnd + 4);

    const std::vector<std::string> lines = strutil::Split(headerBlock, '\n');
    if (lines.empty())
    {
        earlyResponse = MakeErrorResponse(400, "empty request");
        return true;
    }

    const std::vector<std::string> requestLine = strutil::Split(strutil::Trim(lines[0]), ' ');
    if (requestLine.size() < 3)
    {
        earlyResponse = MakeErrorResponse(400, "malformed request line");
        return true;
    }

    request.method = strutil::Trim(requestLine[0]);
    request.target = strutil::Trim(requestLine[1]);
    request.version = strutil::Trim(requestLine[2]);

    for (size_t i = 1; i < lines.size(); ++i)
    {
        const std::string line = strutil::Trim(lines[i]);
        if (line.empty())
        {
            continue;
        }

        const size_t separator = line.find(':');
        if (separator == std::string::npos)
        {
            continue;
        }

        const std::string key = strutil::ToLowerAscii(strutil::Trim(line.substr(0, separator)));
        const std::string value = strutil::Trim(line.substr(separator + 1));
        if (!key.empty())
        {
            request.headers[key] = value;
        }
    }

    const size_t queryStart = request.target.find('?');
    if (queryStart == std::string::npos)
    {
        request.path = request.target;
    }
    else
    {
        request.path = request.target.substr(0, queryStart);
        request.query = request.target.substr(queryStart + 1);
    }

    const size_t fragmentStart = request.path.find('#');
    if (fragmentStart != std::string::npos)
    {
        request.path = request.path.substr(0, fragmentStart);
    }

    if (request.path.empty() || request.path[0] != '/')
    {
        const size_t schemeEnd = request.path.find("://");
        if (schemeEnd != std::string::npos)
        {
            const size_t pathStart = request.path.find('/', schemeEnd + 3);
            request.path = pathStart == std::string::npos ? std::string("/") : request.path.substr(pathStart);
        }
        else
        {
            request.path = "/" + request.path;
        }
    }

    if (request.method != "GET" && request.method != "HEAD")
    {
        earlyResponse = MakeErrorResponse(405, "only GET and HEAD are supported");
        earlyResponse.closeConnection = true;
        return true;
    }

    // тела запросов не поддерживаются
    std::map<std::string, std::string>::const_iterator contentLength = request.headers.find("content-length");
    if (contentLength != request.headers.end() && contentLength->second != "0")
    {
        earlyResponse = MakeErrorResponse(400, "request body is not supported");
        earlyResponse.closeConnection = true;
        return true;
    }

    return true;

}

bool HttpServer::sendResponse(SOCKET clientSocket, const HttpRequest &request, const HttpResponse &response,
                              bool headOnly, bool closeConnection)
{
    (void)request;

    std::string payload;
    payload += strutil::Format("HTTP/1.1 %d %s\r\n", response.status, response.reason.c_str());
    payload += "Content-Type: " + response.contentType + "\r\n";
    payload += strutil::Format("Content-Length: %zu\r\n", response.body.size());
    payload += std::string("Connection: ") + (closeConnection ? "close" : "keep-alive") + "\r\n";
    payload += "Cache-Control: no-store\r\n";
    payload += "Access-Control-Allow-Origin: *\r\n";
    payload += "Server: OpcDaToHttp/1.0.0\r\n";

    for (size_t i = 0; i < response.extraHeaders.size(); ++i)
    {
        payload += response.extraHeaders[i].first + ": " + response.extraHeaders[i].second + "\r\n";
    }

    payload += "\r\n";

    if (!headOnly)
    {
        payload += response.body;
    }

    size_t sent = 0;
    while (sent < payload.size())
    {
        const int written = ::send(clientSocket, payload.c_str() + sent, static_cast<int>(payload.size() - sent), 0);
        if (written <= 0)
        {
            LOGF_DEBUG("HTTP: send() прерван, код %d", ::WSAGetLastError());
            return false;
        }

        sent += static_cast<size_t>(written);
    }

    return true;

}

namespace
{

std::string NameFromPath(const std::string &path, const std::string &prefix)
{
    const std::string encoded = path.substr(prefix.size());
    std::string decoded;
    if (!strutil::UrlDecode(encoded, decoded))
    {
        return encoded;
    }

    return decoded;

}

std::string TagToJson(const TagValue &value, bool includeName, unsigned long staleMs)
{
    const int64_t age =
        value.updatedTickMs != 0 ? static_cast<int64_t>(::GetTickCount64() - value.updatedTickMs) : static_cast<int64_t>(-1);
    const bool stale = age >= 0 && static_cast<unsigned long>(age) > staleMs;

    std::string json = "{";
    if (includeName)
    {
        json += "\"name\":\"" + strutil::JsonEscape(value.name) + "\",";
    }

    json += "\"value\":" + (value.valueJson.empty() ? std::string("null") : value.valueJson) + ",";
    json += "\"type\":\"" + value.type + "\",";
    if (value.isArray)
    {
        json += strutil::Format("\"length\":%zu,\"truncated\":%s,", value.arrayLength, Boolean(value.truncated).c_str());
    }

    json += strutil::Format("\"quality\":%u,", static_cast<unsigned>(value.quality));
    json += "\"qualityText\":\"" + strutil::JsonEscape(value.qualityText) + "\",";

    const std::string timestamp = opcfmt::FileTimeToIso8601(value.timestamp);
    json += "\"timestamp\":";
    json += timestamp.empty() ? std::string("null") : ("\"" + timestamp + "\"");

    json += strutil::Format(",\"ageMs\":%lld,", static_cast<long long>(age));
    json += "\"stale\":" + Boolean(stale) + ",";
    json += "\"hasValue\":" + Boolean(value.hasValue) + ",";
    json += "\"error\":";
    json += value.error == S_OK ? std::string("null") : ("\"" + opcfmt::HResultToString(value.error) + "\"");
    json += "}";

    return json;

}

/// Подсказка по имени при 404
std::string UnknownItemJson(const TagStore &store, const std::string &name)
{
    std::string json = "{\"error\":\"unknown item\",\"name\":\"" + strutil::JsonEscape(name) + "\"";

    const std::vector<std::string> suggestions = store.suggestNames(name, 5);
    if (!suggestions.empty())
    {
        json += ",\"suggestions\":[";
        for (size_t i = 0; i < suggestions.size(); ++i)
        {
            if (i > 0)
            {
                json += ",";
            }

            json += "\"" + strutil::JsonEscape(suggestions[i]) + "\"";
        }

        json += "]";
    }

    json += ",\"hint\":\"GET /items - полный список тегов\"}";
    return json;

}

}

HttpResponse HttpServer::route(const HttpRequest &request)
{
    const std::string &path = request.path;

    if (path == "/" || path == "/help" || path == "/index.html")
    {
        return helpResponse();
    }

    if (path == "/health")
    {
        return healthResponse();
    }

    if (path == "/items")
    {
        return itemsResponse();
    }

    if (path == "/dump")
    {
        return dumpResponse();
    }

    if (path == "/favicon.ico")
    {
        return MakeTextResponse(404, "not found");
    }

    const std::map<std::string, std::vector<std::string>> query = ParseQuery(request.query);

    bool forceRefresh = false;
    std::map<std::string, std::vector<std::string>>::const_iterator refreshParam = query.find("refresh");
    if (refreshParam != query.end() && !refreshParam->second.empty())
    {
        forceRefresh = IsTruthy(refreshParam->second.front());
    }

    std::vector<std::string> names;
    std::map<std::string, std::vector<std::string>>::const_iterator nameParam = query.find("name");
    if (nameParam != query.end())
    {
        names = nameParam->second;
    }

    if (path == "/value" || strutil::StartsWith(path, "/value/"))
    {
        std::string name = names.empty() ? std::string() : names.front();
        if (name.empty() && strutil::StartsWith(path, "/value/"))
        {
            name = NameFromPath(path, "/value/");
        }

        return valueResponse(name, forceRefresh);
    }

    if (path == "/raw" || strutil::StartsWith(path, "/raw/"))
    {
        std::string name = names.empty() ? std::string() : names.front();
        if (name.empty() && strutil::StartsWith(path, "/raw/"))
        {
            name = NameFromPath(path, "/raw/");
        }

        return rawResponse(name, forceRefresh);
    }

    if (path == "/values")
    {
        return valuesResponse(names, forceRefresh);
    }

    return MakeErrorResponse(404, "unknown endpoint", path);

}

HttpResponse HttpServer::valueResponse(const std::string &name, bool forceRefresh)
{
    if (name.empty())
    {
        return MakeErrorResponse(400, "missing 'name' parameter", "пример: /value?name=Test.Int32");
    }

    if (forceRefresh)
    {
        if (!opc_.isConnected())
        {
            return MakeErrorResponse(503, "opc not connected", opc_.lastError());
        }

        if (!opc_.requestRefresh(config_.requestTimeoutMs))
        {
            return MakeErrorResponse(504, "opc refresh timeout");
        }
    }

    const std::shared_ptr<const TagValue> value = store_.lookup(name);
    if (!value)
    {
        return MakeJsonResponse(404, UnknownItemJson(store_, name));
    }

    return MakeJsonResponse(200, TagToJson(*value, true, config_.staleTimeoutMs()));

}

HttpResponse HttpServer::rawResponse(const std::string &name, bool forceRefresh)
{
    if (name.empty())
    {
        return MakeTextResponse(400, "missing 'name' parameter, example: /raw?name=Test.Int32");
    }

    if (forceRefresh)
    {
        if (!opc_.isConnected())
        {
            return MakeTextResponse(503, "opc not connected");
        }

        if (!opc_.requestRefresh(config_.requestTimeoutMs))
        {
            return MakeTextResponse(504, "opc refresh timeout");
        }
    }

    const std::shared_ptr<const TagValue> value = store_.lookup(name);
    if (!value)
    {
        return MakeTextResponse(404, "unknown item: " + name);
    }

    const bool stale = value->updatedTickMs != 0 &&
                       (::GetTickCount64() - value->updatedTickMs) > config_.staleTimeoutMs();

    HttpResponse response;
    response.status = 200;
    response.reason = "OK";
    response.contentType = "text/plain; charset=utf-8";
    response.body = value->valueText;
    response.extraHeaders.push_back(std::make_pair(std::string("X-Opc-Name"), value->name));
    response.extraHeaders.push_back(std::make_pair(std::string("X-Opc-Type"), value->type));
    response.extraHeaders.push_back(
        std::make_pair(std::string("X-Opc-Quality"), strutil::Format("%u", static_cast<unsigned>(value->quality))));
    response.extraHeaders.push_back(std::make_pair(std::string("X-Opc-Quality-Text"), value->qualityText));
    response.extraHeaders.push_back(
        std::make_pair(std::string("X-Opc-Timestamp"), opcfmt::FileTimeToIso8601(value->timestamp)));
    response.extraHeaders.push_back(std::make_pair(std::string("X-Opc-Stale"), Boolean(stale)));
    if (value->error != S_OK)
    {
        response.extraHeaders.push_back(
            std::make_pair(std::string("X-Opc-Error"), opcfmt::HResultToString(value->error)));
    }

    return response;

}

HttpResponse HttpServer::valuesResponse(const std::vector<std::string> &names, bool forceRefresh)
{
    if (names.empty())
    {
        return MakeErrorResponse(400, "missing 'name' parameter", "пример: /values?name=Test.Int32&name=Test.Float");
    }

    if (forceRefresh)
    {
        if (!opc_.isConnected())
        {
            return MakeErrorResponse(503, "opc not connected", opc_.lastError());
        }

        if (!opc_.requestRefresh(config_.requestTimeoutMs))
        {
            return MakeErrorResponse(504, "opc refresh timeout");
        }
    }

    const unsigned long staleMs = config_.staleTimeoutMs();
    std::string valuesJson = "{";
    std::string missingJson;
    size_t found = 0;

    for (size_t i = 0; i < names.size(); ++i)
    {
        const std::shared_ptr<const TagValue> value = store_.lookup(names[i]);
        if (!value)
        {
            if (!missingJson.empty())
            {
                missingJson += ",";
            }

            missingJson += "\"" + strutil::JsonEscape(names[i]) + "\"";
            continue;
        }

        if (found > 0)
        {
            valuesJson += ",";
        }

        valuesJson += "\"" + strutil::JsonEscape(value->name) + "\":" + TagToJson(*value, false, staleMs);
        ++found;
    }

    valuesJson += "}";

    std::string json = "{\"count\":" + strutil::Format("%zu", found) +
                       ",\"requested\":" + strutil::Format("%zu", names.size()) + ",\"values\":" + valuesJson;
    if (!missingJson.empty())
    {
        json += ",\"missing\":[" + missingJson + "]";
    }

    json += "}";

    return MakeJsonResponse(found > 0 ? 200 : 404, json);

}

HttpResponse HttpServer::itemsResponse()
{
    const std::vector<std::string> names = store_.names();

    std::string json = "{\"count\":" + strutil::Format("%zu", names.size()) + ",\"items\":[";
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i > 0)
        {
            json += ",";
        }

        json += "\"" + strutil::JsonEscape(names[i]) + "\"";
    }

    json += "]}";
    return MakeJsonResponse(200, json);

}

HttpResponse HttpServer::dumpResponse()
{
    const std::vector<std::shared_ptr<const TagValue>> values = store_.all();
    const unsigned long staleMs = config_.staleTimeoutMs();

    std::string json = "{\"count\":" + strutil::Format("%zu", values.size()) + ",\"values\":{";
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (values[i] == nullptr)
        {
            continue;
        }

        if (i > 0)
        {
            json += ",";
        }

        json += "\"" + strutil::JsonEscape(values[i]->name) + "\":" + TagToJson(*values[i], false, staleMs);
    }

    json += "}}";
    return MakeJsonResponse(200, json);

}

HttpResponse HttpServer::healthResponse()
{
    const size_t tags = store_.nameCount();
    const size_t values = store_.valueCount();
    const int64_t age = store_.ageMs();
    const bool connected = opc_.isConnected();

    std::string status = "ok";
    int httpStatus = 200;
    if (!connected)
    {
        status = "down";
        httpStatus = 503;
    }
    else if (values == 0)
    {
        status = "degraded";
    }

    const std::string serverProgId = opc_.serverProgId();

    std::string json = "{\"status\":\"" + status + "\"";
    json += ",\"opcConnected\":" + Boolean(connected);
    json += ",\"server\":";
    json += serverProgId.empty() ? std::string("null") : ("\"" + strutil::JsonEscape(serverProgId) + "\"");
    json += ",\"host\":\"" + strutil::JsonEscape(strutil::ToUtf8(config_.host)) + "\"";
    json += strutil::Format(",\"port\":%u", static_cast<unsigned>(boundPort_));
    json += strutil::Format(",\"serverState\":%lu", static_cast<unsigned long>(opc_.serverState()));
    json += strutil::Format(",\"tags\":%zu", tags);
    json += strutil::Format(",\"values\":%zu", values);
    json += strutil::Format(",\"items\":%zu", opc_.itemCount());
    json += strutil::Format(",\"lastUpdateAgeMs\":%lld", static_cast<long long>(age));
    json += strutil::Format(",\"serverContactAgeMs\":%lld", static_cast<long long>(opc_.serverContactAgeMs()));
    json += strutil::Format(",\"snapshotVersion\":%llu", static_cast<unsigned long long>(store_.version()));
    json += strutil::Format(",\"refreshAckId\":%llu", static_cast<unsigned long long>(opc_.refreshAckId()));
    json += strutil::Format(",\"uptimeSec\":%llu",
                            static_cast<unsigned long long>((::GetTickCount64() - startTick_) / 1000));
    json += ",\"lastError\":";
    const std::string lastError = opc_.lastError();
    json += lastError.empty() ? std::string("null") : ("\"" + strutil::JsonEscape(lastError) + "\"");

    std::string connections;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connections = std::to_string(activeConnections_.load());
    }

    json += ",\"connections\":" + connections;
    json += ",\"version\":\"1.0.0\"}";

    return MakeJsonResponse(httpStatus, json);

}

HttpResponse HttpServer::helpResponse()
{
    const std::string serverProgId = opc_.serverProgId();
    const size_t tags = store_.nameCount();
    const size_t values = store_.valueCount();

    std::string text;
    text += "OpcDaToHttp 1.0.0 - мост OPC DA -> HTTP\n";
    text += "\n";
    text += "OPC хост:     " + strutil::ToUtf8(config_.host) + "\n";
    text += "OPC сервер:   " + (serverProgId.empty() ? std::string("(ещё не подключён)") : serverProgId) + "\n";
    text += "Состояние:    " + std::string(opc_.isConnected() ? "подключён" : "нет связи") + "\n";
    text += "Тегов:        " + strutil::Format("%zu", tags) + ", со значениями: " + strutil::Format("%zu", values) + "\n";
    text += "Период OPC:   " + strutil::Format("%lu", config_.rateMs) + " мс\n";
    text += "HTTP:         слушает на всех сетевых интерфейсах (0.0.0.0), порт " + strutil::Format("%u", static_cast<unsigned>(boundPort_)) + "\n";
    text += "\n";
    text += "Эндпоинты:\n";
    text += "  GET /                        эта справка\n";
    text += "  GET /health                  состояние (JSON, 503 если нет связи с OPC)\n";
    text += "  GET /items                   список известных тегов\n";
    text += "  GET /value?name=<ТЕГ>        значение тега (JSON)\n";
    text += "  GET /value/<ТЕГ>             то же, имя тега в самом URL\n";
    text += "  GET /raw?name=<ТЕГ>          только значение (text/plain)\n";
    text += "  GET /values?name=A&name=B    несколько тегов сразу\n";
    text += "  GET /dump                    значения всех известных тегов\n";
    text += "\n";
    text += "Параметры запроса:\n";
    text += "  ?refresh=1                   перед ответом дождаться обновления значений от сервера\n";
    text += "                               (OPC-поток выполнит refresh, ожидание до " +
            strutil::Format("%lu", config_.requestTimeoutMs) + " мс)\n";
    text += "\n";
    text += "Коды ответов: 200 - успех, 400 - нет параметра name, 404 - тег не найден,\n";
    text += "              405 - метод не поддерживается, 503 - нет связи с OPC, 504 - таймаут refresh\n";
    text += "\n";
    text += "Примеры:\n";
    text += "  curl \"http://localhost:" + strutil::Format("%u", static_cast<unsigned>(boundPort_)) +
            "/value?name=Test.Int32\"\n";
    text += "  curl \"http://localhost:" + strutil::Format("%u", static_cast<unsigned>(boundPort_)) +
            "/raw/Test.Float\"\n";
    text += "  powershell: (Invoke-WebRequest \"http://localhost:" +
            strutil::Format("%u", static_cast<unsigned>(boundPort_)) + "/raw?name=Test.Int32\").Content\n";

    return MakeTextResponse(200, text);

}
