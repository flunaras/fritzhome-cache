#include "server.h"
#include "router.h"
#include "types.h"
#include "logger.h"

#include <httplib.h>
#include <sstream>
#include <algorithm>
#include <cctype>

// ── In-memory self-signed certificate (OpenSSL 3.x) ──────────────────────────
//
// Only compiled when OpenSSL support is compiled in (it always is, but the
// #ifdef keeps the code well-scoped and avoids ODR issues in unit-test builds
// that might compile this TU without CPPHTTPLIB_OPENSSL_SUPPORT).

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT

// RAII wrapper for a self-signed X.509 certificate and its RSA private key.
//
// SSL_CTX_use_certificate / SSL_CTX_use_PrivateKey (called by the SSLServer
// constructor) increment the OpenSSL reference counts on both objects, so it
// is safe to destroy this wrapper as soon as the SSLServer has been created.
struct SelfSignedCert {
    X509*     cert{nullptr};
    EVP_PKEY* key{nullptr};

    SelfSignedCert() = default;
    SelfSignedCert(const SelfSignedCert&) = delete;
    SelfSignedCert& operator=(const SelfSignedCert&) = delete;

    // Move: zero out the source so its destructor is a no-op
    SelfSignedCert(SelfSignedCert&& o) noexcept : cert(o.cert), key(o.key) {
        o.cert = nullptr;
        o.key  = nullptr;
    }

    ~SelfSignedCert() {
        if (cert) X509_free(cert);
        if (key)  EVP_PKEY_free(key);
    }

    bool valid() const { return cert != nullptr && key != nullptr; }
};

// Generates an RSA-2048 private key and a self-signed X.509 v3 certificate
// valid for 365 days.  Returns an object where valid() == true on success.
// thread-safe — uses only OpenSSL re-entrant APIs.
static SelfSignedCert make_self_signed_cert() {
    SelfSignedCert sc;

    // ── Private key: RSA-2048 ──────────────────────────────────────────────
    {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (!ctx) return sc;
        bool ok = (EVP_PKEY_keygen_init(ctx) > 0 &&
                   EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) > 0 &&
                   EVP_PKEY_keygen(ctx, &sc.key) > 0);
        EVP_PKEY_CTX_free(ctx);
        if (!ok) return sc;
    }

    // ── Certificate ────────────────────────────────────────────────────────
    sc.cert = X509_new();
    if (!sc.cert) return sc;  // destructor frees sc.key

    X509_set_version(sc.cert, 2);  // 2 = v3

    ASN1_INTEGER_set(X509_get_serialNumber(sc.cert), 1L);

    // Validity: now → now + 365 days
    X509_gmtime_adj(X509_get_notBefore(sc.cert), 0L);
    X509_gmtime_adj(X509_get_notAfter(sc.cert),  365L * 24 * 3600);

    X509_set_pubkey(sc.cert, sc.key);

    // Subject and issuer are identical for a self-signed certificate
    X509_NAME* name = X509_get_subject_name(sc.cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("fritzhome-cache"), -1, -1, 0);
    X509_set_issuer_name(sc.cert, name);

    if (!X509_sign(sc.cert, sc.key, EVP_sha256())) {
        X509_free(sc.cert);
        sc.cert = nullptr;
    }
    return sc;
}

#endif  // CPPHTTPLIB_OPENSSL_SUPPORT

// ── Request conversion helpers ────────────────────────────────────────────────

static std::string pct_encode_param(const std::string& s) {
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

// Reconstruct the full path-with-query-string from a parsed httplib::Request.
// httplib URL-decodes the params, so we must re-encode them for the upstream.
static std::string build_path_and_query(const httplib::Request& req) {
    if (req.params.empty()) return req.path;
    std::string pq = req.path + '?';
    bool first = true;
    for (const auto& [k, v] : req.params) {
        if (!first) pq += '&';
        first = false;
        pq += pct_encode_param(k);
        if (!v.empty()) {
            pq += '=';
            pq += pct_encode_param(v);
        }
    }
    return pq;
}

static ProxyRequest to_proxy_request(const httplib::Request& req) {
    ProxyRequest pr;
    pr.method         = req.method;
    pr.path           = req.path;
    pr.path_and_query = build_path_and_query(req);
    pr.body           = req.body;
    for (const auto& [k, v] : req.headers)
        pr.headers[k] = v;
    return pr;
}

static void apply_proxy_response(const ProxyResponse& pr, httplib::Response& res) {
    res.status = pr.status;
    res.body   = pr.body;
    for (const auto& [k, v] : pr.headers)
        res.set_header(k.c_str(), v.c_str());
    // Ensure Content-Type is set if body is non-empty and no type was provided
    if (!res.body.empty() && res.get_header_value("Content-Type").empty())
        res.set_header("Content-Type", "application/octet-stream");
}

// ── HttpServer::run_impl ──────────────────────────────────────────────────────

template<typename Svr>
void HttpServer::run_impl(Svr& svr, const Config& cfg, RequestRouter& router) {
    Logger& log = Logger::instance();

    // Configure thread pool
    const int n_threads = cfg.threads;
    svr.new_task_queue = [n_threads]() -> httplib::TaskQueue* {
        return new httplib::ThreadPool(n_threads);
    };

    // Catch-all handler for all HTTP methods
    auto dispatch = [&router, &log](const httplib::Request& req,
                                    httplib::Response& res) {
        try {
            ProxyRequest  pr   = to_proxy_request(req);
            ProxyResponse resp = router.route(pr);
            apply_proxy_response(resp, res);
        } catch (const std::exception& ex) {
            log.error(std::string("Worker exception: ") + ex.what());
            res.status = 500;
            res.set_content(R"({"error":"internal_server_error"})",
                            "application/json");
        } catch (...) {
            log.error("Worker exception: unknown");
            res.status = 500;
            res.set_content(R"({"error":"internal_server_error"})",
                            "application/json");
        }
    };

    svr.Get    (".*", dispatch);
    svr.Post   (".*", dispatch);
    svr.Put    (".*", dispatch);
    svr.Delete (".*", dispatch);
    svr.Patch  (".*", dispatch);
    svr.Options(".*", dispatch);

    // Register the server pointer so stop() can reach it
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_svr = &svr;  // SSLServer inherits from Server — upcast is valid
    }

    const std::string tls_tag = !cfg.tls_cert.empty()   ? " [HTTPS]"
                              : cfg.tls_self_signed      ? " [HTTPS/self-signed]"
                              :                            " [HTTP]";
    log.info("Listening on " + cfg.listen_addr + ":" +
             std::to_string(cfg.listen_port) + tls_tag);

    if (!svr.listen(cfg.listen_addr.c_str(), cfg.listen_port)) {
        log.error("Failed to bind on " + cfg.listen_addr + ":" +
                  std::to_string(cfg.listen_port));
    }

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_svr = nullptr;
    }
}

// ── HttpServer::run ───────────────────────────────────────────────────────────

void HttpServer::run(const Config& cfg, RequestRouter& router) {
    Logger& log = Logger::instance();

    const bool use_tls_files = !cfg.tls_cert.empty() && !cfg.tls_key.empty();

    if (use_tls_files) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        httplib::SSLServer svr(cfg.tls_cert.c_str(), cfg.tls_key.c_str());
        if (!svr.is_valid()) {
            log.error("Failed to load TLS certificate (" + cfg.tls_cert +
                      ") or key (" + cfg.tls_key + ")");
            return;
        }
        run_impl(svr, cfg, router);
#else
        log.error("Built without OpenSSL — HTTPS listener unavailable");
#endif
    } else if (cfg.tls_self_signed) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        SelfSignedCert sc = make_self_signed_cert();
        if (!sc.valid()) {
            log.error("Failed to generate self-signed TLS certificate");
            return;
        }
        log.info("Generated in-memory self-signed TLS certificate "
                 "(RSA-2048, SHA-256, valid 365 days)");
        // SSLServer increments the OpenSSL refcounts on cert and key;
        // sc's destructor will safely decrement them when it goes out of scope.
        httplib::SSLServer svr(sc.cert, sc.key);
        if (!svr.is_valid()) {
            log.error("Failed to initialise TLS context from self-signed certificate");
            return;
        }
        run_impl(svr, cfg, router);
#else
        log.error("Built without OpenSSL — HTTPS listener unavailable");
#endif
    } else {
        httplib::Server svr;
        run_impl(svr, cfg, router);
    }
}

// ── HttpServer::stop ──────────────────────────────────────────────────────────

void HttpServer::stop() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_svr) {
        m_svr->stop();
        Logger::instance().info("Server stop requested");
    }
}
