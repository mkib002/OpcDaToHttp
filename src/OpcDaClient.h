/*
OpcDaToHttp

OPC-клиент: единственный поток, владеющий объектами OPCClientToolKit.
Подключается к серверу, создаёт активную группу со всеми тегами, включает
подписку (enableAsync) и складывает полученные значения в TagStore.

HTTP-код обращается к OPC только через requestRefresh(): он лишь просит поток
выполнить refresh, значения же HTTP-потоки читают из TagStore напрямую.
*/
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AppConfig.h"
#include "OPCClient.h"
#include "OPCGroup.h"
#include "OPCHost.h"
#include "OPCItem.h"
#include "OPCServer.h"
#include "TagStore.h"

class OpcDaClient : public IAsyncDataCallback
{
  public:
    OpcDaClient(const AppConfig &config, TagStore &store);

    ~OpcDaClient();

    /// Запустить OPC-поток
    void start();

    /// Остановить OPC-поток и дождаться его завершения
    void stop();

    /// Ждать первой попытки подключения (true - если подключение удалось)
    bool waitUntilReady(unsigned long timeoutMs);

    bool isConnected() const
    {
        return connected_.load();
    }

    bool isStopped() const
    {
        return stopped_.load();
    }

    std::string serverProgId() const;

    std::string lastError() const;

    /// Состояние сервера (OPC_STATUS_*), 0xFFFFFFFF - неизвестно
    DWORD serverState() const
    {
        return serverState_.load();
    }

    /// Сколько мс назад была успешная связь с сервером (getStatus), -1 - ещё не было
    int64_t serverContactAgeMs() const;

    size_t itemCount() const
    {
        return itemCount_.load();
    }

    /// Номер последнего выполненного запроса на refresh
    uint64_t refreshAckId() const
    {
        return refreshAckId_.load();
    }

    /// Попросить OPC-поток выполнить внеочередной refresh и дождаться его выполнения
    bool requestRefresh(unsigned long timeoutMs);

    /// Вызывается тулкитом из OPC-потока (подписка на изменения)
    void OnDataChange(COPCGroup &group, COPCItemDataMap &changes) override;

  private:
    void threadMain();
    bool connectSession();
    void disconnectSession();
    COPCGroup *createGroup(unsigned long &revisedRate);
    bool loadItemNames(std::vector<std::wstring> &names, std::vector<std::string> &namesUtf8);
    bool loadItemNamesFromFile(std::vector<std::wstring> &names, std::vector<std::string> &namesUtf8);
    bool addItemsInChunks(const std::vector<std::wstring> &names, const std::vector<std::string> &namesUtf8,
                          std::vector<std::string> &addedNames);
    void subscribeAndPrime();
    void runSession();
    void doRefresh();
    void pumpMessages();
    bool pumpUntilCompleted(CTransaction *transaction, unsigned long timeoutMs);
    bool publishItemData(OPCHANDLE handle, OPCItemData *data);
    size_t publishItemDataMap(COPCItemDataMap &dataMap);
    void enumerateLocalServers();
    bool checkServerState();
    void waitInterruptible(unsigned long timeoutMs) const;
    void setError(const std::string &text);
    void setServerProgId(const std::string &progId);

    AppConfig config_;
    TagStore &store_;

    COPCHost *host_ = nullptr;
    COPCServer *server_ = nullptr;
    COPCGroup *group_ = nullptr;
    std::vector<COPCItem *> items_;

    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopped_{false};
    std::atomic<bool> refreshRequested_{false};
    std::atomic<bool> subscriptionActive_{false};
    std::atomic<uint64_t> refreshRequestId_{0};
    std::atomic<uint64_t> refreshAckId_{0};
    std::atomic<size_t> itemCount_{0};
    std::atomic<DWORD> serverState_{0xFFFFFFFF};
    std::atomic<uint64_t> lastServerContactTick_{0};
    unsigned long groupSequence_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable readyCv_;
    std::condition_variable refreshCv_;
    std::string serverProgId_;
    std::string lastError_;

}; // OpcDaClient
