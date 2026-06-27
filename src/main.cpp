#include "config.h"
#include "logger.h"
#include "cache.h"
#include "fritzClient.h"
#include "router.h"
#include "server.h"

#include <CLI/CLI.hpp>

#include <atomic>
#include <csignal>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

// ── Signal handling ───────────────────────────────────────────────────────────

static std::atomic<bool> g_stop{false};

static void on_signal(int) {
    g_stop.store(true);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    Config cfg;
    int    verbose_count      = 0;
    int    log_level_override = -1;

    CLI::App app{
        "fritzhome-cache — transparent Fritz!Box smart home API cache proxy"};
    app.set_version_flag("--version", "1.0.0");

    // Config file support — CLI11 reads the file before applying CLI args,
    // so command-line arguments always override values from the config file.
    // The default path is optional: if the file is absent, built-in defaults
    // apply.  Pass --config /other/path to use a different file (must exist).
    app.set_config("--config",
                   "/etc/fritzhome-cache/fritzhome-cache.conf",
                   "Path to configuration file\n"
                   "(default: /etc/fritzhome-cache/fritzhome-cache.conf)");

    app.add_option("-a,--fritzbox-addr", cfg.fritzbox_addr,
                   "Fritz!Box hostname or IP (default: fritz.box)");
    app.add_option("-s,--fritzbox-scheme", cfg.fritzbox_scheme,
                   "Upstream scheme: http or https (default: http)")
        ->check(CLI::IsMember({"http", "https"}));
    app.add_option("-p,--listen-port", cfg.listen_port,
                   "Port to listen on (default: 8080)");
    app.add_option("-l,--listen-addr", cfg.listen_addr,
                   "Address to bind to (default: 127.0.0.1)");
    app.add_option("--tls-cert", cfg.tls_cert,
                   "PEM certificate file — enables HTTPS listener");
    app.add_option("--tls-key", cfg.tls_key,
                   "PEM private key file — enables HTTPS listener");
    app.add_flag("--tls-self-signed", cfg.tls_self_signed,
                 "Generate an in-memory self-signed certificate at startup "
                 "(enables HTTPS; mutually exclusive with --tls-cert/--tls-key)");
    app.add_option("-t,--ttl", cfg.ttl,
                   "Default cache TTL in seconds (default: 5)");
    app.add_option("--ttl-overview", cfg.ttl_overview,
                   "TTL override for /api/v0/smarthome/overview");
    app.add_option("--ttl-units", cfg.ttl_units,
                   "TTL override for /api/v0/smarthome/overview/units/*");
    app.add_option("-j,--threads", cfg.threads,
                   "Worker thread count (default: 4)");
    app.add_option("--sweep-interval", cfg.sweep_interval,
                   "Cache sweeper interval in seconds (default: max(1, ttl/2))");
    app.add_flag("-v,--verbose", verbose_count,
                 "Increase log verbosity (repeat up to 5×: -vvvvv = TRACE)");
    app.add_option("--log-level", log_level_override,
                   "Set log level 0–5 directly (overrides -v count)")
        ->check(CLI::Range(0, 5));
    app.add_flag("--verify-tls", cfg.verify_tls,
                 "Verify Fritz!Box TLS certificate (default: off)");
    app.add_option("--shutdown-timeout", cfg.shutdown_timeout,
                   "Graceful shutdown timeout in seconds (default: 5)");

    CLI11_PARSE(app, argc, argv);

    // ── Resolve log level ──────────────────────────────────────────────────
    // -v count →  0:ERROR  1:WARN  2:INFO  3:DEBUG  4+:TRACE
    if (log_level_override >= 0) {
        cfg.log_level = log_level_override;
    } else {
        cfg.log_level = std::min(verbose_count + 1, 5);
        // verbose_count==0 → 1 (ERROR) by default
    }
    Logger::instance().set_level(cfg.log_level);
    Logger& log = Logger::instance();

    // ── Validate TLS flags ────────────────────────────────────────────────
    bool has_cert = !cfg.tls_cert.empty();
    bool has_key  = !cfg.tls_key.empty();
    if (cfg.tls_self_signed && (has_cert || has_key)) {
        log.error("--tls-self-signed cannot be combined with --tls-cert or --tls-key");
        return 1;
    }
    if (has_cert != has_key) {
        log.error("--tls-cert and --tls-key must both be provided together");
        return 1;
    }
    if (has_cert) {
        if (!std::ifstream(cfg.tls_cert).good()) {
            log.error("Cannot read TLS certificate: " + cfg.tls_cert);
            return 1;
        }
        if (!std::ifstream(cfg.tls_key).good()) {
            log.error("Cannot read TLS key: " + cfg.tls_key);
            return 1;
        }
    }

    // ── Resolve derived config values ─────────────────────────────────────
    if (cfg.sweep_interval < 0)
        cfg.sweep_interval = std::max(1, cfg.ttl / 2);

    // ── Startup log ───────────────────────────────────────────────────────
    log.info("fritzhome-cache v1.0.0 starting");
    log.info("Upstream : " + cfg.fritzbox_scheme + "://" + cfg.fritzbox_addr);
    log.info("Listener : " + cfg.listen_addr + ":" +
             std::to_string(cfg.listen_port) +
             (has_cert             ? " (HTTPS)"
              : cfg.tls_self_signed ? " (HTTPS/self-signed)"
              :                      " (HTTP)"));
    log.info("Cache TTL: " + std::to_string(cfg.ttl) + "s" +
             (cfg.ttl_overview >= 0
                  ? "  overview=" + std::to_string(cfg.ttl_overview) + "s"
                  : "") +
             (cfg.ttl_units >= 0
                  ? "  units=" + std::to_string(cfg.ttl_units) + "s"
                  : "") +
             "  sweep=" + std::to_string(cfg.sweep_interval) + "s" +
             "  threads=" + std::to_string(cfg.threads));

    // ── Build components ──────────────────────────────────────────────────
    CacheLayer    cache(cfg.ttl, cfg.sweep_interval);
    FritzClient   fritz(cfg);
    RequestRouter router(cfg, cache, fritz);
    HttpServer    server;

    // ── Signal handling ───────────────────────────────────────────────────
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // ── Run server in background thread ───────────────────────────────────
    std::promise<void> server_exited;
    auto server_future = server_exited.get_future();

    std::thread server_thread([&]() {
        try {
            server.run(cfg, router);
        } catch (const std::exception& ex) {
            Logger::instance().error(std::string("Server thread threw: ") + ex.what());
        } catch (...) {
            Logger::instance().error("Server thread threw unknown exception");
        }
        server_exited.set_value();
    });

    // ── Wait for stop signal or server self-exit ──────────────────────────
    while (!g_stop.load()) {
        if (server_future.wait_for(std::chrono::milliseconds(200)) ==
            std::future_status::ready) {
            // Server exited on its own (bind failure, etc.)
            break;
        }
    }

    if (g_stop.load()) {
        log.info("Shutdown signal received — stopping (timeout " +
                 std::to_string(cfg.shutdown_timeout) + "s)");
        server.stop();
        // Give in-flight requests time to complete
        if (server_future.wait_for(
                std::chrono::seconds(cfg.shutdown_timeout)) !=
            std::future_status::ready) {
            log.warn("Shutdown timeout exceeded; forcing exit");
        }
    }

    server_thread.join();
    log.info("Shutdown complete");
    return 0;
}
