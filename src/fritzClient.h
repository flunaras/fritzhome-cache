#pragma once
#include "config.h"
#include "types.h"

// FritzClient — thin upstream HTTP/HTTPS client.
// thread-safe: creates a fresh connection per call (no shared state).
class FritzClient {
public:
    explicit FritzClient(const Config& cfg);

    // thread-safe; creates a new connection per call
    // Forwards the request to the Fritz!Box and returns the raw response.
    // On network error / timeout resp.upstream_error is true.
    [[nodiscard]] ProxyResponse forward(const ProxyRequest& req) const;

private:
    std::string m_base_url;  // e.g. "http://fritz.box" or "https://fritz.box:443"
    bool        m_verify_tls;
};
