#include "OpcDaClient.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>

#include "Logger.h"
#include "OpcValueFormatter.h"
#include "StringUtil.h"

namespace
{

const unsigned long kStatusCheckPeriodMs = 5000; // период проверки состояния сервера
const unsigned long kRefreshTimeoutMs = 5000;    // ожидание завершения refresh
const size_t kAddItemsChunkSize = 256;           // тегов за один вызов AddItems
const HRESULT kOpcDuplicateName = static_cast<HRESULT>(0xC004000C); // OPC_E_DUPLICATENAME (в opcda.h тулкита не определён)

}

OpcDaClient::OpcDaClient(const AppConfig &config, TagStore &store) : config_(config), store_(store)
{
}

OpcDaClient::~OpcDaClient()
{
    stop();

}

void OpcDaClient::start()
{
    stopRequested_.store(false);
    stopped_.store(false);
    thread_ = std::thread(&OpcDaClient::threadMain, this);

}

void OpcDaClient::stop()
{
    stopRequested_.store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
    }

    readyCv_.notify_all();
    refreshCv_.notify_all();

    if (thread_.joinable())
    {
        thread_.join();
    }

    stopped_.store(true);

}

bool OpcDaClient::waitUntilReady(unsigned long timeoutMs)
{
    std::unique_lock<std::mutex> lock(mutex_);
    const bool ready = readyCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]() {
        return ready_.load() || stopped_.load();
    });
    lock.unlock();

    return ready && connected_.load();

}

std::string OpcDaClient::serverProgId() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return serverProgId_;

}

std::string OpcDaClient::lastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;

}

int64_t OpcDaClient::serverContactAgeMs() const
{
    const uint64_t last = lastServerContactTick_.load(std::memory_order_relaxed);
    if (last == 0)
    {
        return static_cast<int64_t>(-1);
    }

    return static_cast<int64_t>(::GetTickCount64() - last);

}

bool OpcDaClient::requestRefresh(unsigned long timeoutMs)
{
    if (stopped_.load() || stopRequested_.load() || !connected_.load())
    {
        return false;
    }

    const uint64_t requestId = refreshRequestId_.fetch_add(1) + 1;
    refreshRequested_.store(true);

    std::unique_lock<std::mutex> lock(mutex_);
    refreshCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this, requestId]() {
        return refreshAckId_.load() >= requestId || stopRequested_.load();
    });

    return refreshAckId_.load() >= requestId;

}

void OpcDaClient::setError(const std::string &text)
{
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_ = text;

}

void OpcDaClient::setServerProgId(const std::string &progId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    serverProgId_ = progId;

}

void OpcDaClient::OnDataChange(COPCGroup &group, COPCItemDataMap &changes)
{
    (void)group;

    size_t published = 0;

    // ВАЖНО: ключ читаем до GetNextValue (см. OPCItemData.h / API.md тулкита)
    POSITION pos = changes.GetStartPosition();
    while (pos)
    {
        const OPCHANDLE handle = changes.GetKeyAt(pos);
        OPCItemData *data = changes.GetNextValue(pos);
        if (publishItemData(handle, data))
        {
            ++published;
        }
    }

    if (published > 0)
    {
        LOGF_DEBUG("OPC: подписка принесла обновлений: %zu (версия кэша %llu)", published,
                   static_cast<unsigned long long>(store_.version()));
    }

}

bool OpcDaClient::publishItemData(OPCHANDLE handle, OPCItemData *data)
{
    if (data == nullptr)
    {
        return false;
    }

    COPCItem *item = data->item();
    if (item == nullptr && group_ != nullptr)
    {
        // сервер вернул неизвестный клиентский handle - пробуем найти элемент группы
        group_->lookupOpcItem(handle, item);
    }

    if (item == nullptr)
    {
        LOGF_WARN("OPC: получены данные по неизвестному handle %lu (пропущены)", static_cast<unsigned long>(handle));
        return false;
    }

    const opcfmt::FormatResult formatted = opcfmt::Format(data->vDataValue);

    TagValue tag;
    tag.name = strutil::ToUtf8(item->getName());
    tag.quality = data->wQuality;
    tag.error = data->Error;
    tag.timestamp = data->ftTimeStamp;
    tag.type = formatted.type;
    tag.valueJson = formatted.valueJson;
    tag.valueText = formatted.valueText;
    tag.isArray = formatted.isArray;
    tag.arrayLength = formatted.arrayLength;
    tag.truncated = formatted.truncated;
    tag.hasValue = SUCCEEDED(data->Error);
    tag.qualityText = opcfmt::QualityToString(data->wQuality);
    tag.updatedTickMs = ::GetTickCount64();

    store_.publish(tag);
    return true;

}

size_t OpcDaClient::publishItemDataMap(COPCItemDataMap &dataMap)
{
    size_t published = 0;

    POSITION pos = dataMap.GetStartPosition();
    while (pos)
    {
        const OPCHANDLE handle = dataMap.GetKeyAt(pos);
        OPCItemData *data = dataMap.GetNextValue(pos);
        if (publishItemData(handle, data))
        {
            ++published;
        }
    }

    return published;

}

void OpcDaClient::threadMain()
{
    try
    {
        if (!COPCClient::init())
        {
            setError("COPCClient::init() вернул false");
            ready_.store(true);
            readyCv_.notify_all();
            stopped_.store(true);
            return;
        }
    }
    catch (OPCException &ex)
    {
        setError("COPCClient::init: " + strutil::ToUtf8(ex.reasonString()));
        LOGF_ERROR("OPC: инициализация COM не удалась: %s", strutil::ToUtf8(ex.reasonString()).c_str());
        ready_.store(true);
        readyCv_.notify_all();
        stopped_.store(true);
        return;
    }
    catch (std::exception &ex)
    {
        setError(std::string("COPCClient::init: ") + ex.what());
        LOGF_ERROR("OPC: инициализация COM не удалась: %s", ex.what());
        ready_.store(true);
        readyCv_.notify_all();
        stopped_.store(true);
        return;
    }

    LOGF_INFO("OPC: COM инициализирован в потоке %lu", static_cast<unsigned long>(::GetCurrentThreadId()));

    if (config_.listServers)
    {
        enumerateLocalServers();
        ready_.store(true);
        readyCv_.notify_all();
        COPCClient::stop();
        stopped_.store(true);
        readyCv_.notify_all();
        return;
    }

    while (!stopRequested_.load())
    {
        if (connectSession())
        {
            connected_.store(true);
            setError("");
            ready_.store(true);
            readyCv_.notify_all();

            LOGF_INFO("OPC: подключено к '%s', тегов: %zu, период группы: %lu мс", serverProgId().c_str(),
                      itemCount_.load(), config_.rateMs);

            runSession();

            connected_.store(false);
            disconnectSession();
        }
        else
        {
            ready_.store(true);
            readyCv_.notify_all();

            if (stopRequested_.load() || config_.reconnectSec == 0)
            {
                break;
            }

            LOGF_WARN("OPC: подключение не удалось (%s), повтор через %lu c", lastError().c_str(), config_.reconnectSec);
            waitInterruptible(config_.reconnectSec * 1000);
        }
    }

    stopRequested_.store(true);
    COPCClient::stop();
    stopped_.store(true);
    readyCv_.notify_all();
    refreshCv_.notify_all();
    LogInfo("OPC: поток завершён");

}

void OpcDaClient::waitInterruptible(unsigned long timeoutMs) const
{
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    while (!stopRequested_.load() && ::GetTickCount64() < deadline)
    {
        ::Sleep(20);
    }

}

void OpcDaClient::pumpMessages()
{
    MSG message;
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
    {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

}

bool OpcDaClient::pumpUntilCompleted(CTransaction *transaction, unsigned long timeoutMs)
{
    if (transaction == nullptr)
    {
        return false;
    }

    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    while (!transaction->isCompleted() && !stopRequested_.load())
    {
        pumpMessages();
        if (transaction->isCompleted())
        {
            break;
        }

        if (::GetTickCount64() >= deadline)
        {
            break;
        }

        ::Sleep(5);
    }

    pumpMessages();
    return transaction->isCompleted();

}

void OpcDaClient::doRefresh()
{
    if (group_ == nullptr || stopRequested_.load())
    {
        return;
    }

    if (!group_->asyncIO2Supported())
    {
        // Сервер не поддерживает асинхронный I/O 2.0 (например, Mera.Recorder) -
        // читаем значения синхронно через IOPCSyncIO.
        try
        {
            COPCItemDataMap dataMap;
            group_->readSync(items_, dataMap, OPC_DS_CACHE);
            const size_t published = publishItemDataMap(dataMap);
            LOGF_DEBUG("OPC: sync read принёс значений: %zu", published);
        }
        catch (OPCException &ex)
        {
            LOGF_WARN("OPC: sync read не удался: %s", strutil::ToUtf8(ex.reasonString()).c_str());
        }
        catch (std::exception &ex)
        {
            LOGF_WARN("OPC: sync read не удался: %s", ex.what());
        }
        return;
    }

    CTransaction *transaction = nullptr;
    try
    {
        transaction = group_->refresh(OPC_DS_CACHE, nullptr);
    }
    catch (OPCException &ex)
    {
        LOGF_WARN("OPC: refresh не удался: %s", strutil::ToUtf8(ex.reasonString()).c_str());
        return;
    }
    catch (std::exception &ex)
    {
        LOGF_WARN("OPC: refresh не удался: %s", ex.what());
        return;
    }

    if (transaction == nullptr)
    {
        return;
    }

    if (!pumpUntilCompleted(transaction, kRefreshTimeoutMs))
    {
        LogWarn("OPC: refresh не завершился за отведённое время");
    }

    // При refresh тулкит не вызывает IAsyncDataCallback - значения лежат в карте транзакции
    const size_t published = publishItemDataMap(transaction->getItemDataMap());
    LOGF_DEBUG("OPC: refresh принёс значений: %zu", published);

    group_->deleteTransaction(transaction);

}

bool OpcDaClient::checkServerState()
{
    if (server_ == nullptr)
    {
        return false;
    }

    try
    {
        ServerStatus status = {0};
        server_->getStatus(status);
        serverState_.store(static_cast<DWORD>(status.dwServerState));
        lastServerContactTick_.store(::GetTickCount64(), std::memory_order_relaxed);

        if (status.dwServerState != OPC_STATUS_RUNNING)
        {
            LOGF_WARN("OPC: сервер в состоянии %lu (не RUNNING)", static_cast<unsigned long>(status.dwServerState));
        }

        return true;
    }
    catch (OPCException &ex)
    {
        LOGF_WARN("OPC: getStatus не удался: %s", strutil::ToUtf8(ex.reasonString()).c_str());
        return false;
    }
    catch (std::exception &ex)
    {
        LOGF_WARN("OPC: getStatus не удался: %s", ex.what());
        return false;
    }

}

void OpcDaClient::runSession()
{
    const bool hasSubscription = subscriptionActive_.load();
    ULONGLONG nextStatusCheck = ::GetTickCount64() + kStatusCheckPeriodMs;
    ULONGLONG nextRefresh =
        config_.refreshSec > 0 ? ::GetTickCount64() + config_.refreshSec * 1000ULL : static_cast<ULONGLONG>(0);
    // Без подписки значения читаем синхронно с периодом группы.
    ULONGLONG nextPoll = hasSubscription ? static_cast<ULONGLONG>(0) : ::GetTickCount64() + config_.rateMs;

    checkServerState();

    while (!stopRequested_.load())
    {
        pumpMessages();

        if (refreshRequested_.exchange(false))
        {
            const uint64_t requestId = refreshRequestId_.load();
            doRefresh();
            refreshAckId_.store(requestId);
            refreshCv_.notify_all();
        }

        if (stopRequested_.load())
        {
            break;
        }

        const ULONGLONG now = ::GetTickCount64();

        if (now >= nextStatusCheck)
        {
            nextStatusCheck = now + kStatusCheckPeriodMs;
            if (!checkServerState())
            {
                LogWarn("OPC: связь с сервером потеряна, переподключение");
                break;
            }
        }

        if (!hasSubscription && now >= nextPoll)
        {
            nextPoll = now + config_.rateMs;
            doRefresh();
        }

        if (config_.refreshSec > 0 && now >= nextRefresh)
        {
            nextRefresh = now + config_.refreshSec * 1000ULL;
            doRefresh();
        }

        ::Sleep(10);
    }

    // разбудить тех, кто ждёт refresh на момент остановки сессии
    refreshAckId_.store(refreshRequestId_.load());
    refreshCv_.notify_all();

}

bool OpcDaClient::connectSession()
{
    try
    {
        const std::wstring host = config_.host.empty() ? std::wstring(L"localhost") : config_.host;
        host_ = COPCClient::makeHost(host);
        if (host_ == nullptr)
        {
            setError("COPCClient::makeHost() вернул nullptr");
            return false;
        }

        std::wstring progId = config_.serverProgId;
        if (progId.empty())
        {
            std::vector<std::wstring> progIds;
            std::vector<CLSID> classIds;
            host_->getListOfDAServers(COPCClient::CATID_OPCDAv20, progIds, classIds);
            if (progIds.empty())
            {
                setError("в системе не найдено OPC DA 2.0 серверов (задайте --server)");
                return false;
            }

            progId = progIds.front();
            LOGF_INFO("OPC: ключ --server не задан, использую первый найденный сервер: '%s'",
                      strutil::ToUtf8(progId).c_str());
        }

        server_ = host_->connectDAServer(progId);
        if (server_ == nullptr)
        {
            setError("connectDAServer() вернул nullptr");
            return false;
        }

        setServerProgId(strutil::ToUtf8(progId));

        {
            ServerStatus status = {0};
            try
            {
                server_->getStatus(status);
                LOGF_INFO("OPC: состояние сервера '%s': %lu (RUNNING=%lu)", strutil::ToUtf8(progId).c_str(),
                          static_cast<unsigned long>(status.dwServerState),
                          static_cast<unsigned long>(OPC_STATUS_RUNNING));
            }
            catch (OPCException &ex)
            {
                LOGF_WARN("OPC: getStatus не удался: %s", strutil::ToUtf8(ex.reasonString()).c_str());
            }
            catch (std::exception &ex)
            {
                LOGF_WARN("OPC: getStatus не удался: %s", ex.what());
            }
        }

        std::vector<std::wstring> names;
        std::vector<std::string> namesUtf8;
        if (!loadItemNames(names, namesUtf8))
        {
            return false;
        }

        unsigned long revisedRate = 0;
        group_ = createGroup(revisedRate);
        if (group_ == nullptr)
        {
            setError("makeGroup() вернул nullptr");
            return false;
        }

        LOGF_DEBUG("OPC: группа создана, запрошенный период %lu мс, фактический %lu мс", config_.rateMs, revisedRate);

        std::vector<std::string> addedNames;
        if (!addItemsInChunks(names, namesUtf8, addedNames))
        {
            setError("не удалось добавить ни одного тега");
            return false;
        }

        // в кэш имён попадают только те теги, которые реально приняты сервером
        store_.setNames(addedNames);

        subscribeAndPrime();
        return true;
    }
    catch (OPCException &ex)
    {
        setError(strutil::ToUtf8(ex.reasonString()));
        LOGF_ERROR("OPC: ошибка подключения (hr=0x%08X): %s", static_cast<unsigned>(ex.hr()),
                   strutil::ToUtf8(ex.reasonString()).c_str());
    }
    catch (std::exception &ex)
    {
        setError(ex.what());
        LOGF_ERROR("OPC: ошибка подключения: %s", ex.what());
    }

    disconnectSession();
    return false;

}

COPCGroup *OpcDaClient::createGroup(unsigned long &revisedRate)
{
    // Попытка 1: фиксированное имя группы.
    try
    {
        return server_->makeGroup(L"HttpBridge", true, config_.rateMs, revisedRate, config_.deadBand);
    }
    catch (OPCException &ex)
    {
        if (ex.hr() != kOpcDuplicateName)
        {
            throw;
        }

        LOGF_WARN("OPC: группа 'HttpBridge' уже существует (DUPLICATENAME), удаляю и создаю заново");
    }

    // Удаляем «застрявшую» группу и пробуем снова с тем же именем.
    server_->removeGroupByName(L"HttpBridge");
    try
    {
        return server_->makeGroup(L"HttpBridge", true, config_.rateMs, revisedRate, config_.deadBand);
    }
    catch (OPCException &ex)
    {
        if (ex.hr() != kOpcDuplicateName)
        {
            throw;
        }

        LOGF_WARN("OPC: повторно DUPLICATENAME - группа недоступна для удаления, использую уникальное имя");
    }

    // Страховка: уникальное имя (PID + порядковый номер), если удаление не помогло.
    const std::wstring unique = L"HttpBridge-" + std::to_wstring(static_cast<unsigned long>(::GetCurrentProcessId())) +
                                L"-" + std::to_wstring(++groupSequence_);
    return server_->makeGroup(unique, true, config_.rateMs, revisedRate, config_.deadBand);

}

void OpcDaClient::disconnectSession()
{
    connected_.store(false);
    serverState_.store(0xFFFFFFFF);

    if (group_ != nullptr)
    {
        try
        {
            group_->disableAsync();
        }
        catch (OPCException &ex)
        {
            LOGF_DEBUG("OPC: disableAsync: %s", strutil::ToUtf8(ex.reasonString()).c_str());
        }
        catch (std::exception &ex)
        {
            LOGF_DEBUG("OPC: disableAsync: %s", ex.what());
        }

        for (size_t i = 0; i < items_.size(); ++i)
        {
            delete items_[i];
        }

        items_.clear();
        delete group_;
        group_ = nullptr;
    }

    itemCount_.store(0);

    if (server_ != nullptr)
    {
        delete server_;
        server_ = nullptr;
    }

    if (host_ != nullptr)
    {
        delete host_;
        host_ = nullptr;
    }

}

void OpcDaClient::enumerateLocalServers()
{
    try
    {
        const std::wstring host = config_.host.empty() ? std::wstring(L"localhost") : config_.host;
        COPCHost *opcHost = COPCClient::makeHost(host);

        std::vector<std::wstring> progIds;
        std::vector<CLSID> classIds;
        opcHost->getListOfDAServers(COPCClient::CATID_OPCDAv20, progIds, classIds);

        if (progIds.empty())
        {
            LogWarn("OPC DA 2.0 серверы не найдены");
        }
        else
        {
            LOGF_INFO("Найдено OPC DA 2.0 серверов: %zu", progIds.size());
            for (size_t i = 0; i < progIds.size(); ++i)
            {
                LOGF_INFO("  [%zu] %s", i + 1, strutil::ToUtf8(progIds[i]).c_str());
            }
        }

        delete opcHost;
    }
    catch (OPCException &ex)
    {
        LOGF_ERROR("OPC: обзор серверов не удался (hr=0x%08X): %s", static_cast<unsigned>(ex.hr()),
                   strutil::ToUtf8(ex.reasonString()).c_str());
    }
    catch (std::exception &ex)
    {
        LOGF_ERROR("OPC: обзор серверов не удался: %s", ex.what());
    }

}

bool OpcDaClient::loadItemNamesFromFile(std::vector<std::wstring> &names, std::vector<std::string> &namesUtf8)
{
    std::ifstream stream(config_.itemsFile.c_str());
    if (!stream.is_open())
    {
        setError("не удалось открыть файл со списком тегов: " + config_.itemsFile);
        LOGF_ERROR("OPC: не удалось открыть файл '%s'", config_.itemsFile.c_str());
        return false;
    }

    std::string line;
    bool firstLine = true;
    while (std::getline(stream, line))
    {
        if (firstLine)
        {
            firstLine = false;
            // снимаем UTF-8 BOM, если файл сохранён с меткой (например, PowerShell Set-Content -Encoding UTF8)
            if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
            {
                line.erase(0, 3);
            }
        }

        const std::string trimmed = strutil::Trim(line);
        if (trimmed.empty() || trimmed[0] == '#')
        {
            continue;
        }

        if (config_.maxItems > 0 && names.size() >= config_.maxItems)
        {
            break;
        }

        names.push_back(strutil::ToWide(trimmed));
        namesUtf8.push_back(trimmed);
    }

    if (names.empty())
    {
        setError("файл со списком тегов пуст: " + config_.itemsFile);
        return false;
    }

    LOGF_INFO("OPC: из файла '%s' прочитано тегов: %zu", config_.itemsFile.c_str(), names.size());
    return true;

}

bool OpcDaClient::loadItemNames(std::vector<std::wstring> &names, std::vector<std::string> &namesUtf8)
{
    names.clear();
    namesUtf8.clear();

    if (!config_.itemsFile.empty())
    {
        return loadItemNamesFromFile(names, namesUtf8);
    }

    std::vector<std::wstring> browsed;
    if (server_ == nullptr || !server_->getItemNames(browsed))
    {
        setError("не удалось прочитать namespace сервера (getItemNames)");
        LogWarn("OPC: namespace недоступен, список тегов можно задать ключом --items-file");
        return false;
    }

    for (size_t i = 0; i < browsed.size(); ++i)
    {
        const std::string nameUtf8 = strutil::ToUtf8(browsed[i]);
        if (!config_.filter.empty() && !strutil::ContainsIgnoreCaseAscii(nameUtf8, config_.filter))
        {
            continue;
        }

        if (config_.maxItems > 0 && names.size() >= config_.maxItems)
        {
            break;
        }

        names.push_back(browsed[i]);
        namesUtf8.push_back(nameUtf8);
    }

    if (names.empty())
    {
        setError("список тегов пуст (проверьте --filter/--max-items)");
        return false;
    }

    LOGF_INFO("OPC: из namespace выбрано тегов: %zu (всего %zu)", names.size(), browsed.size());
    return true;

}

bool OpcDaClient::addItemsInChunks(const std::vector<std::wstring> &names, const std::vector<std::string> &namesUtf8,
                                   std::vector<std::string> &addedNames)
{
    size_t failed = 0;
    addedNames.clear();

    for (size_t offset = 0; offset < names.size(); offset += kAddItemsChunkSize)
    {
        const size_t count = (std::min)(kAddItemsChunkSize, names.size() - offset);
        std::vector<std::wstring> chunk(names.begin() + offset, names.begin() + offset + count);
        std::vector<COPCItem *> items;
        std::vector<HRESULT> errors;

        try
        {
            group_->addItems(chunk, items, errors, true);
        }
        catch (OPCException &ex)
        {
            LOGF_WARN("OPC: AddItems для блока из %zu тегов не удался (hr=0x%08X): %s", count,
                      static_cast<unsigned>(ex.hr()), strutil::ToUtf8(ex.reasonString()).c_str());
            failed += count;
            continue;
        }
        catch (std::exception &ex)
        {
            LOGF_WARN("OPC: AddItems для блока из %zu тегов не удался: %s", count, ex.what());
            failed += count;
            continue;
        }

        for (size_t i = 0; i < items.size(); ++i)
        {
            if (items[i] != nullptr)
            {
                items_.push_back(items[i]);

                const size_t nameIndex = offset + i;
                if (nameIndex < namesUtf8.size())
                {
                    addedNames.push_back(namesUtf8[nameIndex]);
                }
            }
        }

        for (size_t i = 0; i < errors.size() && i < chunk.size(); ++i)
        {
            if (FAILED(errors[i]))
            {
                ++failed;
                LOGF_WARN("OPC: тег '%s' не добавлен (%s)", strutil::ToUtf8(chunk[i]).c_str(),
                          opcfmt::HResultToString(errors[i]).c_str());
            }
        }
    }

    itemCount_.store(items_.size());
    LOGF_INFO("OPC: добавлено тегов: %zu, ошибок: %zu", items_.size(), failed);
    return !items_.empty();

}

void OpcDaClient::subscribeAndPrime()
{
    if (group_ == nullptr)
    {
        return;
    }

    subscriptionActive_.store(false);
    try
    {
        if (!group_->enableAsync(this))
        {
            LogWarn("OPC: подписка недоступна (enableAsync вернул false) - читаю значения периодически");
        }
        else
        {
            subscriptionActive_.store(true);
            LogInfo("OPC: подписка на изменения группы включена");
        }
    }
    catch (OPCException &ex)
    {
        LOGF_WARN("OPC: подписка недоступна (%s) - читаю значения периодически",
                  strutil::ToUtf8(ex.reasonString()).c_str());
    }
    catch (std::exception &ex)
    {
        LOGF_WARN("OPC: подписка недоступна (%s) - читаю значения периодически", ex.what());
    }

    // Начальное заполнение кэша: при подписке значения придут позже, при её
    // отсутствии - синхронным чтением всех тегов.
    doRefresh();

}
