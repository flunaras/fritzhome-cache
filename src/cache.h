#pragma once
#include "types.h"

#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <atomic>

// ── CacheLayer ────────────────────────────────────────────────────────────────
//
// Thread-safe in-memory TTL cache with in-flight deduplication.
//
// Cache key: normalised path — query parameters sorted alphabetically, the
// `sid` parameter excluded.  All authenticated clients share cache entries
// because the Fritz!Box smart home API returns identical device-state data to
// every valid session.
//
// In-flight deduplication:
//   The first thread that observes a cache miss marks the entry as IN_FLIGHT
//   and returns nullopt so it can issue an upstream request.  Every subsequent
//   thread that looks up the same key while IN_FLIGHT is true blocks on a
//   condition_variable.  When the first thread calls store() or store_error()
//   the condvar is notified and all waiters re-check the entry.

class CacheLayer {
public:
    // default_ttl     – seconds; used when store() callers pass ttl=-1
    // sweep_interval  – seconds between background eviction runs
    explicit CacheLayer(int default_ttl, int sweep_interval);
    ~CacheLayer();

    // thread-safe; acquires m_mutex (releases it while blocked on condvar)
    // Returns the cached response on HIT.
    // Returns nullopt on MISS: the entry is marked IN_FLIGHT and the caller
    // MUST subsequently call store() or store_error() for the same key.
    [[nodiscard]] std::optional<ProxyResponse> lookup(std::string_view raw_path);

    // thread-safe; acquires m_mutex
    // Stores a successful upstream response and wakes all waiters.
    void store(std::string_view raw_path, ProxyResponse resp, int ttl_s);

    // thread-safe; acquires m_mutex
    // Marks the entry as failed and wakes all waiters (they will get a brief
    // ERROR cooldown to avoid thundering-herd on a down upstream).
    void store_error(std::string_view raw_path);

    // thread-safe; acquires m_mutex
    // Removes every entry whose normalised key starts with path_prefix.
    // Waiters on flushed entries are woken and will retry as new misses.
    void flush_prefix(std::string_view path_prefix);

    // thread-safe; acquires m_mutex
    // Removes the single entry for the exact normalised key.
    void flush(std::string_view raw_path);

    // Pure function — no lock needed.
    // Normalises a raw path+query string into a stable cache key:
    //   • query parameters sorted alphabetically
    //   • `sid` parameter stripped
    [[nodiscard]] static std::string normalise(std::string_view raw_path);

private:
    enum class EntryStatus { IN_FLIGHT, READY, FAILED };

    struct Entry {
        ProxyResponse                         resp;
        std::chrono::steady_clock::time_point expires_at{};
        EntryStatus                           status{EntryStatus::IN_FLIGHT};
    };

    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::unordered_map<std::string, Entry> m_cache;

    int              m_default_ttl;
    int              m_sweep_interval;
    std::thread      m_sweeper;
    std::atomic_bool m_stop{false};
};
