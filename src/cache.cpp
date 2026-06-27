#include "cache.h"
#include "logger.h"

#include <algorithm>
#include <sstream>
#include <vector>

// ── Path normalisation ────────────────────────────────────────────────────────

std::string CacheLayer::normalise(std::string_view raw) {
    // Split on '?'
    auto q = raw.find('?');
    std::string path(q == std::string_view::npos ? raw : raw.substr(0, q));
    std::string_view query_sv = (q == std::string_view::npos)
                                    ? std::string_view{}
                                    : raw.substr(q + 1);

    if (query_sv.empty()) return path;

    // Parse query parameters, skip 'sid', collect the rest
    std::vector<std::string> params;
    while (!query_sv.empty()) {
        auto amp = query_sv.find('&');
        std::string_view token = (amp == std::string_view::npos)
                                     ? query_sv
                                     : query_sv.substr(0, amp);
        query_sv = (amp == std::string_view::npos)
                       ? std::string_view{}
                       : query_sv.substr(amp + 1);

        if (token.empty()) continue;
        auto eq  = token.find('=');
        auto key = (eq == std::string_view::npos) ? token : token.substr(0, eq);
        if (key == "sid") continue;
        params.emplace_back(token);
    }

    if (params.empty()) return path;

    std::sort(params.begin(), params.end());

    std::string result = path + '?';
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) result += '&';
        result += params[i];
    }
    return result;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

CacheLayer::CacheLayer(int default_ttl, int sweep_interval)
    : m_default_ttl(default_ttl), m_sweep_interval(sweep_interval)
{
    m_sweeper = std::thread([this]() {
        Logger& log = Logger::instance();
        while (!m_stop.load()) {
            // Sleep in small chunks so shutdown is responsive
            auto wake = std::chrono::steady_clock::now() +
                        std::chrono::seconds(m_sweep_interval);
            while (!m_stop.load() && std::chrono::steady_clock::now() < wake) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (m_stop.load()) break;

            auto now   = std::chrono::steady_clock::now();
            int  swept = 0;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                for (auto it = m_cache.begin(); it != m_cache.end(); ) {
                    auto& e = it->second;
                    if (e.status != EntryStatus::IN_FLIGHT && now >= e.expires_at) {
                        ++swept;
                        it = m_cache.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (swept > 0 && log.is_enabled(LogLevel::DBG))
                log.debug("CACHE SWEEP removed " + std::to_string(swept) +
                          " expired entries");
        }
    });
}

CacheLayer::~CacheLayer() {
    m_stop.store(true);
    m_cv.notify_all();
    if (m_sweeper.joinable()) m_sweeper.join();
}

// ── lookup ────────────────────────────────────────────────────────────────────

std::optional<ProxyResponse> CacheLayer::lookup(std::string_view raw_path) {
    const std::string key = normalise(raw_path);
    Logger& log = Logger::instance();

    std::unique_lock<std::mutex> lk(m_mutex);
    const auto timeout = std::chrono::seconds(30); // max wait for in-flight

    while (true) {
        auto it = m_cache.find(key);

        // ── Not in cache ────────────────────────────────────────────────────
        if (it == m_cache.end()) {
            // Register as in-flight; caller must issue the upstream request.
            m_cache.emplace(key, Entry{});
            if (log.is_enabled(LogLevel::DBG))
                log.debug("CACHE MISS  key=" + key);
            return std::nullopt;
        }

        Entry& e = it->second;

        // ── In-flight: wait ─────────────────────────────────────────────────
        if (e.status == EntryStatus::IN_FLIGHT) {
            if (log.is_enabled(LogLevel::DBG))
                log.debug("CACHE DEDUP key=" + key + " (waiting)");
            auto cv_result = m_cv.wait_for(lk, timeout);
            if (cv_result == std::cv_status::timeout) {
                // Previous requester appears stuck; take over.
                log.warn("CACHE TIMEOUT key=" + key + " — retrying as miss");
                it = m_cache.find(key);
                if (it != m_cache.end() && it->second.status == EntryStatus::IN_FLIGHT)
                    m_cache.erase(it);
                // Fall through: next iteration creates new IN_FLIGHT entry
            }
            continue; // re-check
        }

        // ── Failed: brief cooldown to prevent thundering herd ───────────────
        if (e.status == EntryStatus::FAILED) {
            auto now = std::chrono::steady_clock::now();
            if (now < e.expires_at) {
                if (log.is_enabled(LogLevel::DBG))
                    log.debug("CACHE ERROR key=" + key + " (cooldown)");
                ProxyResponse err;
                err.upstream_error = true;
                err.status         = 502;
                err.body           = R"({"error":"upstream_unreachable"})";
                err.headers["Content-Type"] = "application/json";
                return err;
            }
            // Cooldown expired — treat as a miss.
            m_cache.erase(it);
            continue;
        }

        // ── READY: check TTL ────────────────────────────────────────────────
        auto now = std::chrono::steady_clock::now();
        if (now >= e.expires_at) {
            if (log.is_enabled(LogLevel::DBG))
                log.debug("CACHE MISS  key=" + key + " (expired)");
            m_cache.erase(it);
            // Insert new in-flight
            m_cache.emplace(key, Entry{});
            return std::nullopt;
        }

        auto ttl_rem = std::chrono::duration_cast<std::chrono::milliseconds>(
            e.expires_at - now).count();
        if (log.is_enabled(LogLevel::DBG))
            log.debug("CACHE HIT   key=" + key +
                      " ttl_remaining=" + std::to_string(ttl_rem) + "ms");
        return e.resp;
    }
}

// ── store ─────────────────────────────────────────────────────────────────────

void CacheLayer::store(std::string_view raw_path, ProxyResponse resp, int ttl_s) {
    const std::string key = normalise(raw_path);
    if (ttl_s <= 0) ttl_s = m_default_ttl;
    const auto expires = std::chrono::steady_clock::now() + std::chrono::seconds(ttl_s);

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto& e      = m_cache[key];   // insert or update
        e.resp       = std::move(resp);
        e.expires_at = expires;
        e.status     = EntryStatus::READY;
    }
    m_cv.notify_all();

    Logger& log = Logger::instance();
    if (log.is_enabled(LogLevel::DBG))
        log.debug("CACHE STORE key=" + key + " ttl=" + std::to_string(ttl_s) + "s");
}

// ── store_error ───────────────────────────────────────────────────────────────

void CacheLayer::store_error(std::string_view raw_path) {
    const std::string key = normalise(raw_path);
    // Brief 2-second cooldown so all current waiters see FAILED, preventing
    // a thundering herd immediately re-hitting a down upstream.
    const auto cooldown = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_cache.find(key);
        if (it != m_cache.end()) {
            it->second.status     = EntryStatus::FAILED;
            it->second.expires_at = cooldown;
        }
    }
    m_cv.notify_all();
    Logger::instance().warn("CACHE FAIL  key=" + key);
}

// ── flush_prefix ──────────────────────────────────────────────────────────────

void CacheLayer::flush_prefix(std::string_view path_prefix) {
    const std::string prefix(path_prefix);
    int flushed = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto it = m_cache.begin(); it != m_cache.end(); ) {
            if (it->first.substr(0, prefix.size()) == prefix) {
                ++flushed;
                it = m_cache.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (flushed) {
        m_cv.notify_all(); // wake any waiters on flushed in-flight entries
        Logger& log = Logger::instance();
        if (log.is_enabled(LogLevel::DBG))
            log.debug("CACHE FLUSH prefix=" + prefix +
                      " removed=" + std::to_string(flushed));
    }
}

// ── flush ─────────────────────────────────────────────────────────────────────

void CacheLayer::flush(std::string_view raw_path) {
    const std::string key = normalise(raw_path);
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        removed = (m_cache.erase(key) > 0);
    }
    if (removed) {
        m_cv.notify_all();
        Logger& log = Logger::instance();
        if (log.is_enabled(LogLevel::DBG))
            log.debug("CACHE FLUSH key=" + key);
    }
}
