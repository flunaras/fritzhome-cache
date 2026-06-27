#include "router.h"
#include "cache.h"
#include "fritzClient.h"
#include "logger.h"

#include <string_view>

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

static bool is_write_method(std::string_view m) {
    return m == "PUT" || m == "POST" || m == "DELETE" || m == "PATCH";
}

// Truncate a SID for trace-level logging: first 4 chars + "…"
static std::string truncate_sid(const Headers& headers) {
    auto it = headers.find("Authorization");
    if (it == headers.end()) return "(none)";
    std::string_view auth = it->second;
    std::string_view prefix = "AVM-SID ";
    if (auth.substr(0, prefix.size()) == prefix) {
        auto sid = auth.substr(prefix.size());
        if (sid.size() > 4) return std::string(sid.substr(0, 4)) + "\xe2\x80\xa6"; // UTF-8 ellipsis
        return std::string(sid);
    }
    return "(none)";
}

// ── RequestRouter ─────────────────────────────────────────────────────────────

RequestRouter::RequestRouter(const Config& cfg, CacheLayer& cache, FritzClient& fritz)
    : m_cfg(cfg), m_cache(cache), m_fritz(fritz)
{}

int RequestRouter::ttl_for(std::string_view path) const {
    // Strip query string for path comparison
    auto path_only = path.substr(0, path.find('?'));

    if (path_only == "/api/v0/smarthome/overview")
        return (m_cfg.ttl_overview >= 0) ? m_cfg.ttl_overview : m_cfg.ttl;

    if (starts_with(path_only, "/api/v0/smarthome/overview/units/"))
        return (m_cfg.ttl_units >= 0) ? m_cfg.ttl_units : m_cfg.ttl;

    return m_cfg.ttl;
}

// ── route ─────────────────────────────────────────────────────────────────────

ProxyResponse RequestRouter::route(const ProxyRequest& req) const {
    Logger& log = Logger::instance();
    const std::string& path   = req.path_and_query;
    const std::string& method = req.method;

    // 1. Auth endpoints — always forward without caching
    if (starts_with(path, "/login_sid.lua")) {
        if (log.is_enabled(LogLevel::DBG))
            log.debug("ROUTE FORWARD (auth) " + method + " " + path);
        return forward_only(req);
    }

    // 2. Write methods — flush affected cache entries then forward
    if (is_write_method(method)) {
        if (log.is_enabled(LogLevel::DBG))
            log.debug("ROUTE FLUSH+FORWARD " + method + " " + path);
        return forward_flush(req);
    }

    // 3. Cacheable smart-home GETs
    if (method == "GET" && starts_with(path, "/api/v0/smarthome/")) {
        if (log.is_enabled(LogLevel::DBG))
            log.debug("ROUTE CACHE  GET " + path +
                      " sid=" + truncate_sid(req.headers));
        return cache_lookup(req);
    }

    // 4. Everything else — pass through
    if (log.is_enabled(LogLevel::DBG))
        log.debug("ROUTE FORWARD " + method + " " + path);
    return forward_only(req);
}

// ── forward_only ──────────────────────────────────────────────────────────────

ProxyResponse RequestRouter::forward_only(const ProxyRequest& req) const {
    return m_fritz.forward(req);
}

// ── forward_flush ─────────────────────────────────────────────────────────────

ProxyResponse RequestRouter::forward_flush(const ProxyRequest& req) const {
    // Flush BEFORE the upstream call (atomic w.r.t. the cache, no lock held
    // during the actual HTTP round-trip).
    // A PUT to any unit should invalidate both that unit and the overview.
    m_cache.flush_prefix("/api/v0/smarthome/overview");
    return m_fritz.forward(req);
}

// ── cache_lookup ──────────────────────────────────────────────────────────────

ProxyResponse RequestRouter::cache_lookup(const ProxyRequest& req) const {
    Logger& log = Logger::instance();
    const std::string& path = req.path_and_query;

    auto cached = m_cache.lookup(path);

    // HIT (or cooldown ERROR from a previous failure)
    if (cached.has_value()) {
        if (cached->upstream_error) {
            // Propagate the cooldown error immediately
            log.warn("ROUTE ERROR  " + path + " (upstream failure cooldown)");
        }
        return *cached;
    }

    // MISS — we are the designated requester (entry is marked IN_FLIGHT)
    auto resp = m_fritz.forward(req);

    if (resp.upstream_error) {
        m_cache.store_error(path);
        return resp;
    }

    // Fritz!Box returned HTTP 401: don't cache; flush the stale entry
    if (resp.status == 401) {
        log.warn("UPSTREAM 401 for " + path + " — flushing cache entry");
        m_cache.flush(path);
        return resp;
    }

    // Store successful response
    m_cache.store(path, resp, ttl_for(path));
    return resp;
}
