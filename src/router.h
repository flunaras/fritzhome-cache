#pragma once
#include "config.h"
#include "types.h"

class CacheLayer;
class FritzClient;

// RequestRouter — classifies every incoming request and returns a response.
//
// Routing table:
//   /login_sid.lua          → ForwardOnly   (auth — never cached)
//   PUT/POST/DELETE/PATCH   → ForwardFlush  (flush cache, then forward)
//   GET /api/v0/smarthome/  → CacheLookup
//   anything else           → ForwardOnly
//
// thread-safe: all state is immutable after construction.
class RequestRouter {
public:
    RequestRouter(const Config& cfg, CacheLayer& cache, FritzClient& fritz);

    // thread-safe; acquires CacheLayer::m_mutex only when needed
    [[nodiscard]] ProxyResponse route(const ProxyRequest& req) const;

private:
    // Resolve per-path TTL from config
    [[nodiscard]] int ttl_for(std::string_view path) const;

    // Forward without touching the cache
    [[nodiscard]] ProxyResponse forward_only(const ProxyRequest& req) const;

    // Flush relevant cache entries then forward
    [[nodiscard]] ProxyResponse forward_flush(const ProxyRequest& req) const;

    // Check cache; on miss forward and store
    [[nodiscard]] ProxyResponse cache_lookup(const ProxyRequest& req) const;

    const Config& m_cfg;
    CacheLayer&   m_cache;
    FritzClient&  m_fritz;
};
