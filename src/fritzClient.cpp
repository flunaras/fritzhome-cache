#include "fritzClient.h"
#include "logger.h"

#include <httplib.h>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>

// ── Hop-by-hop header names (lowercase) ──────────────────────────────────────

static bool is_hop_by_hop(const std::string& name) {
    // Lowercase comparison
    static const char* const HOP[] = {
        "connection", "keep-alive", "transfer-encoding",
        "te", "trailers", "upgrade",
        "proxy-authenticate", "proxy-authorization",
        "content-length",  // httplib sets this itself
        nullptr
    };
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    for (int i = 0; HOP[i]; ++i)
        if (lower == HOP[i]) return true;
    return false;
}

// ── URL percent-encoder (for reconstructing query strings from decoded params)

static std::string pct_encode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

// ── FritzClient ───────────────────────────────────────────────────────────────

FritzClient::FritzClient(const Config& cfg)
    : m_verify_tls(cfg.verify_tls)
{
    m_base_url = cfg.fritzbox_scheme + "://" + cfg.fritzbox_addr;
}

ProxyResponse FritzClient::forward(const ProxyRequest& req) const {
    Logger& log = Logger::instance();

    // Build upstream httplib::Headers (strip hop-by-hop, override Host)
    httplib::Headers hdrs;
    for (const auto& [k, v] : req.headers) {
        if (!is_hop_by_hop(k))
            hdrs.emplace(k, v);
    }
    // Ensure Host matches the upstream, not the proxy's listen address
    hdrs.erase("Host");
    hdrs.erase("host");

    // Extract Content-Type for methods that need it separately
    std::string content_type;
    {
        auto it = hdrs.find("Content-Type");
        if (it == hdrs.end()) {
            // try lowercase
            for (auto& [k, v] : hdrs)
                if (k == "content-type") { content_type = v; break; }
        } else {
            content_type = it->second;
        }
        hdrs.erase("Content-Type");
        hdrs.erase("content-type");
    }
    if (content_type.empty()) content_type = "application/json";

    // Create a fresh client (thread-safe: no shared state)
    httplib::Client cli(m_base_url);
    cli.enable_server_certificate_verification(m_verify_tls);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);
    cli.set_write_timeout(30);

    const std::string& path = req.path_and_query;
    const std::string& method = req.method;
    const std::string& body   = req.body;

    if (log.is_enabled(LogLevel::TRACE))
        log.trace("UPSTREAM    " + method + " " + m_base_url + path);

    auto t0 = std::chrono::steady_clock::now();

    httplib::Result result{nullptr, httplib::Error::Unknown};

    if (method == "GET" || method == "HEAD") {
        result = (method == "GET") ? cli.Get(path, hdrs)
                                   : cli.Head(path, hdrs);
    } else if (method == "POST") {
        result = cli.Post(path, hdrs, body, content_type);
    } else if (method == "PUT") {
        result = cli.Put(path, hdrs, body, content_type);
    } else if (method == "DELETE") {
        result = cli.Delete(path, hdrs, body, content_type);
    } else if (method == "PATCH") {
        result = cli.Patch(path, hdrs, body, content_type);
    } else {
        ProxyResponse err;
        err.upstream_error = true;
        err.status         = 400;
        err.body           = R"({"error":"unsupported_method"})";
        err.headers["Content-Type"] = "application/json";
        return err;
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    if (!result) {
        std::string err_str = httplib::to_string(result.error());
        log.warn("UPSTREAM    " + method + " " + m_base_url + path +
                 " FAILED: " + err_str);
        ProxyResponse err;
        err.upstream_error = true;
        err.status         = 502;
        err.body           = R"({"error":"upstream_unreachable"})";
        err.headers["Content-Type"] = "application/json";
        return err;
    }

    if (log.is_enabled(LogLevel::TRACE))
        log.trace("UPSTREAM    " + std::to_string(result->status) + " " +
                  std::to_string(result->body.size()) + " bytes in " +
                  std::to_string(elapsed_ms) + "ms");

    ProxyResponse resp;
    resp.upstream_error = false;
    resp.status         = result->status;
    resp.body           = result->body;
    for (const auto& [k, v] : result->headers) {
        if (!is_hop_by_hop(k))
            resp.headers[k] = v;
    }
    return resp;
}
