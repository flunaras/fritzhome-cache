#pragma once
#include "config.h"

#include <mutex>
#include <atomic>

// Forward declarations — full httplib.h is only needed in server.cpp
namespace httplib { class Server; }
class RequestRouter;

// HttpServer — listens for incoming connections and dispatches to RequestRouter.
//
// Plain HTTP mode  : neither --tls-cert nor --tls-key given, --tls-self-signed absent (default).
// HTTPS/file mode  : both --tls-cert and --tls-key given → uses httplib::SSLServer.
// HTTPS/self-signed: --tls-self-signed given, no cert/key files → generates an
//                    in-memory RSA-2048 certificate at startup via OpenSSL and
//                    passes it to httplib::SSLServer(X509*, EVP_PKEY*).
//
// Worker threads   : controlled by Config::threads via httplib's ThreadPool.
class HttpServer {
public:
    HttpServer() = default;

    // Blocks until stop() is called or listen fails.
    void run(const Config& cfg, RequestRouter& router);

    // thread-safe — may be called from any thread including a signal watcher.
    void stop();

private:
    // Template so the same setup code works for Server and SSLServer without
    // duplicating the handler registration logic.  Defined in server.cpp.
    template<typename Svr>
    void run_impl(Svr& svr, const Config& cfg, RequestRouter& router);

    // Pointer to the active httplib::Server (or SSLServer, which inherits it).
    // Protected by m_mutex.
    std::mutex       m_mutex;
    httplib::Server* m_svr{nullptr};
};
