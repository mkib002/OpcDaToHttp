/*
OpcDaToHttp

Хранилище значений тегов. Пишет в него только OPC-поток (publish),
читают HTTP-потоки (lookup/all/names). Между ними - shared_ptr на неизменяемое
значение под std::shared_mutex: HTTP-поток удерживает shared_lock лишь на время
копирования указателя, дальше работает с собственной копией shared_ptr.
*/
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * Значение тега после конвертации в строки. VARIANT разбирается в OPC-потоке
 * (в OnDataChange / обработке refresh), поэтому HTTP-потокам не нужен COM и
 * не нужно следить за временем жизни COPCItemDataMap.
 */
struct TagValue
{
    std::string name;                 // каноническое имя тега (UTF-8)
    std::string valueJson;            // готовый JSON-литерал: 42, 3.14, "текст", null, [...]
    std::string valueText;            // текстовое представление для /raw
    std::string type;                 // "VT_I4", "VT_ARRAY|VT_UI2" ...
    std::string qualityText;          // "Good", "Bad - Not Connected" ...
    WORD quality = 0;
    HRESULT error = S_OK;
    FILETIME timestamp = {0, 0};
    bool hasValue = false;            // чтение прошло без ошибки
    bool isArray = false;
    size_t arrayLength = 0;
    bool truncated = false;           // массив отдан не полностью (см. kMaxArrayElements)
    uint64_t updatedTickMs = 0;       // GetTickCount64() в момент получения значения

}; // TagValue

class TagStore
{
  public:
    /// Задать список известных имён (из namespace сервера / items-файла)
    void setNames(const std::vector<std::string> &names);

    /// Опубликовать значение (вызывает только OPC-поток)
    void publish(const TagValue &value);

    /// Точное совпадение имени, затем регистронезависимое
    std::shared_ptr<const TagValue> lookup(const std::string &name) const;

    std::vector<std::shared_ptr<const TagValue>> all() const;

    std::vector<std::string> names() const;

    size_t nameCount() const;

    size_t valueCount() const;

    bool hasName(const std::string &name) const;

    /// Счётчик публикаций: увеличивается на каждое обновление от сервера
    uint64_t version() const
    {
        return version_.load(std::memory_order_relaxed);
    }

    /// Возраст последнего обновления в мс (-1, если обновлений ещё не было)
    int64_t ageMs() const;

    /// Похожие имена для подсказки в 404
    std::vector<std::string> suggestNames(const std::string &name, size_t limit) const;

  private:
    std::shared_ptr<const TagValue> findLocked(const std::string &name) const;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const TagValue>> values_;
    std::unordered_map<std::string, std::string> lowerToCanonical_;
    std::atomic<uint64_t> version_{0};
    std::atomic<uint64_t> lastUpdateTickMs_{0};

}; // TagStore
