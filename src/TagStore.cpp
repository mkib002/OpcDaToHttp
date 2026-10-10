#include "TagStore.h"

#include "StringUtil.h"

void TagStore::setNames(const std::vector<std::string> &names)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    values_.clear();
    lowerToCanonical_.clear();

    for (size_t i = 0; i < names.size(); ++i)
    {
        std::shared_ptr<TagValue> value = std::make_shared<TagValue>();
        value->name = names[i];
        values_[names[i]] = value;
        lowerToCanonical_[strutil::ToLowerAscii(names[i])] = names[i];
    }

}

void TagStore::publish(const TagValue &value)
{
    // Копия принадлежит читателям: после выхода из блокировки её нельзя менять.
    std::shared_ptr<TagValue> copy = std::make_shared<TagValue>(value);

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        std::string canonical = copy->name;
        if (values_.find(canonical) == values_.end())
        {
            const std::string lower = strutil::ToLowerAscii(canonical);
            std::unordered_map<std::string, std::string>::const_iterator known = lowerToCanonical_.find(lower);
            if (known != lowerToCanonical_.end())
            {
                canonical = known->second;
            }
            else
            {
                // имя, которого не было в namespace (например, добавлено вручную)
                lowerToCanonical_[lower] = canonical;
            }
        }

        values_[canonical] = copy;
    }

    version_.fetch_add(1, std::memory_order_relaxed);
    lastUpdateTickMs_.store(::GetTickCount64(), std::memory_order_relaxed);

}

std::shared_ptr<const TagValue> TagStore::lookup(const std::string &name) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    const std::shared_ptr<const TagValue> direct = findLocked(name);
    if (direct)
    {
        return direct;
    }

    // Разделитель в именах тегов у разных серверов разный ('/' или '.'):
    // если точного имени нет, пробуем тот же путь с другим разделителем.
    std::string alternative(name);
    bool changed = false;
    for (size_t i = 0; i < alternative.size(); ++i)
    {
        if (alternative[i] == '.')
        {
            alternative[i] = '/';
            changed = true;
        }
    }

    if (!changed)
    {
        for (size_t i = 0; i < alternative.size(); ++i)
        {
            if (alternative[i] == '/')
            {
                alternative[i] = '.';
                changed = true;
            }
        }
    }

    if (changed)
    {
        return findLocked(alternative);
    }

    return std::shared_ptr<const TagValue>();

}

std::shared_ptr<const TagValue> TagStore::findLocked(const std::string &name) const
{
    std::unordered_map<std::string, std::shared_ptr<const TagValue>>::const_iterator it = values_.find(name);
    if (it != values_.end())
    {
        return it->second;
    }

    std::unordered_map<std::string, std::string>::const_iterator lower =
        lowerToCanonical_.find(strutil::ToLowerAscii(name));
    if (lower == lowerToCanonical_.end())
    {
        return std::shared_ptr<const TagValue>();
    }

    it = values_.find(lower->second);
    if (it == values_.end())
    {
        return std::shared_ptr<const TagValue>();
    }

    return it->second;

}

std::vector<std::shared_ptr<const TagValue>> TagStore::all() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::vector<std::shared_ptr<const TagValue>> result;
    result.reserve(values_.size());
    for (std::unordered_map<std::string, std::shared_ptr<const TagValue>>::const_iterator it = values_.begin();
         it != values_.end(); ++it)
    {
        result.push_back(it->second);
    }

    return result;

}

std::vector<std::string> TagStore::names() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::vector<std::string> result;
    result.reserve(values_.size());
    for (std::unordered_map<std::string, std::shared_ptr<const TagValue>>::const_iterator it = values_.begin();
         it != values_.end(); ++it)
    {
        result.push_back(it->first);
    }

    return result;

}

size_t TagStore::nameCount() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return values_.size();

}

size_t TagStore::valueCount() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    size_t count = 0;
    for (std::unordered_map<std::string, std::shared_ptr<const TagValue>>::const_iterator it = values_.begin();
         it != values_.end(); ++it)
    {
        if (it->second && it->second->hasValue)
        {
            ++count;
        }
    }

    return count;

}

bool TagStore::hasName(const std::string &name) const
{
    return lookup(name) != nullptr;

}

int64_t TagStore::ageMs() const
{
    const uint64_t last = lastUpdateTickMs_.load(std::memory_order_relaxed);
    if (last == 0)
    {
        return -1;
    }

    return static_cast<int64_t>(::GetTickCount64() - last);

}

std::vector<std::string> TagStore::suggestNames(const std::string &name, size_t limit) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::vector<std::string> result;
    if (limit == 0)
    {
        return result;
    }

    for (std::unordered_map<std::string, std::shared_ptr<const TagValue>>::const_iterator it = values_.begin();
         it != values_.end() && result.size() < limit; ++it)
    {
        if (strutil::ContainsIgnoreCaseAscii(it->first, name))
        {
            result.push_back(it->first);
        }
    }

    for (std::unordered_map<std::string, std::shared_ptr<const TagValue>>::const_iterator it = values_.begin();
         it != values_.end() && result.size() < limit; ++it)
    {
        if (strutil::StartsWith(strutil::ToLowerAscii(it->first), strutil::ToLowerAscii(name)))
        {
            bool present = false;
            for (size_t i = 0; i < result.size(); ++i)
            {
                if (result[i] == it->first)
                {
                    present = true;
                    break;
                }
            }

            if (!present)
            {
                result.push_back(it->first);
            }
        }
    }

    return result;

}
