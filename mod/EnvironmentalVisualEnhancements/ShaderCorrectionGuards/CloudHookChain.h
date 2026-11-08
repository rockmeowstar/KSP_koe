#pragma once
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace CloudHookChain
{
struct Endpoint
{
    void *target{}, *body{}, *pending{};
    std::atomic<void *> next{};
};

enum class StageResult { NotOwned, Staged, Conflict };

class Registry
{
    std::mutex mutex;
    std::vector<Endpoint *> endpoints, staged;
public:
    bool Add(Endpoint &endpoint, void *target, void *body)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!target || !body || endpoint.target) return false;
        for (auto *entry : endpoints) if (entry->target == target) return false;
        try { endpoints.push_back(&endpoint); }
        catch (...) { return false; }
        endpoint.target = target;
        endpoint.body = body;
        return true;
    }

    bool Remove(void *target)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto found = std::find_if(endpoints.begin(), endpoints.end(), [=](Endpoint *e) { return e->target == target; });
        if (found == endpoints.end()) return false;
        auto *entry = *found;
        if (entry->next.load(std::memory_order_acquire) || entry->pending) return false;
        entry->target = entry->body = nullptr;
        endpoints.erase(found);
        return true;
    }

    StageResult Stage(void *target, void *replacement, void **original)
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto *entry : endpoints)
        {
            if (entry->target != target) continue;
            if (!replacement || !original || entry->next.load(std::memory_order_acquire) || entry->pending)
                return StageResult::Conflict;
            try { staged.push_back(entry); }
            catch (...) { return StageResult::Conflict; }
            *original = entry->body;
            entry->pending = replacement;
            return StageResult::Staged;
        }
        return StageResult::NotOwned;
    }

    void Commit()
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto *entry : staged)
        {
            entry->next.store(entry->pending, std::memory_order_release);
            entry->pending = nullptr;
        }
        staged.clear();
    }

    void Cancel()
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto *entry : staged) entry->pending = nullptr;
        staged.clear();
    }
};

inline Registry registry;

template<class Function, class... Args>
auto Invoke(const Endpoint &endpoint, Function body, Args... args) -> decltype(body(args...))
{
    auto next = reinterpret_cast<Function>(endpoint.next.load(std::memory_order_acquire));
    if (next) return next(args...);
    return body(args...);
}
}
