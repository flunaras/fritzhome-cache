#pragma once
#include <string>

struct Config {
    // Upstream Fritz!Box
    std::string fritzbox_addr   = "fritz.box";
    std::string fritzbox_scheme = "http";

    // Listener
    std::string listen_addr = "127.0.0.1";
    int         listen_port = 8080;

    // TLS listener — three mutually exclusive modes:
    //   file-based : tls_cert + tls_key both non-empty → load PEM files
    //   self-signed: tls_self_signed == true, tls_cert/tls_key empty → generate in memory
    //   plain HTTP : all three are default
    std::string tls_cert;
    std::string tls_key;
    bool        tls_self_signed = false;

    // Cache TTLs (seconds); -1 = inherit from ttl
    int ttl          = 5;
    int ttl_overview = -1;
    int ttl_units    = -1;

    // Worker threads
    int threads = 4;

    // Sweeper wake interval (seconds); -1 = max(1, ttl/2)
    int sweep_interval = -1;

    // Log verbosity: 0=silent 1=error 2=warn 3=info 4=debug 5=trace
    int log_level = 1;

    // Verify upstream TLS certificate (default: off — Fritz!Box uses self-signed)
    bool verify_tls = false;

    // Graceful-shutdown wait (seconds)
    int shutdown_timeout = 5;
};
