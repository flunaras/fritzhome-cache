# AGENTS.md — fritzhome-cache

## Project Overview

`fritzhome-cache` is a transparent HTTP/HTTPS reverse-proxy and cache that sits
between smart-home clients and a Fritz!Box router. Its sole job is to reduce
load on the Fritz!Box by serving cached responses for read-only API calls, while
passing write commands and authentication requests through unchanged.

Key constraints:
- **No own credentials.** The proxy never stores passwords or session IDs
  long-term. Every Fritz!Box session is established with credentials supplied
  by a connecting client.
- **Transparent.** Clients talk to the proxy using the same Fritz!Box API they
  would use directly. No client-side changes are needed beyond pointing the host
  to the proxy address.
- **Console application written in C++17.**
- **Concurrent.** Multiple clients may issue simultaneous requests. The proxy
  coalesces in-flight upstream requests (request deduplication) and serialises
  cache access with fine-grained locking.

---

## Fritz!Box Smart Home API — Quick Reference

All information below was reverse-engineered from
[flunaras/fritzhome](https://github.com/flunaras/fritzhome).

### Authentication (never cached)

| Step | Method | URL | Notes |
|---|---|---|---|
| 1 | GET | `/login_sid.lua?version=2` | Returns XML `<Challenge>` and current `<SID>` |
| 2 | GET | `/login_sid.lua?version=2&username=…&response=…` | Returns XML with issued `<SID>` |
| 3 | GET | `/login_sid.lua?logout=1&sid=…` | Log out; fire-and-forget |

Challenge variants:
- **PBKDF2** (`"2$iter1$salt1$iter2$salt2"`) — two-pass PBKDF2-HMAC-SHA256.
- **MD5** (legacy) — MD5 of `"<challenge>-<password>"` encoded as UTF-16LE.

All REST calls add `Authorization: AVM-SID <sid>`.

### REST Endpoints (candidates for caching)

| Method | Path | Cacheable | Default TTL |
|---|---|---|---|
| GET | `/api/v0/smarthome/overview` | yes | configurable (default 5 s) |
| GET | `/api/v0/smarthome/overview/units/{unitUID}` | yes | configurable (default 5 s) |
| PUT | `/api/v0/smarthome/overview/units/{unitUID}` | **no** | — |

A successful PUT must flush all cached entries for the affected path(s).

### Authentication Passthrough Model

The proxy does **not** manage Fritz!Box sessions itself. Instead:

1. A client sends an authentication request to the proxy.
2. The proxy forwards it verbatim to the Fritz!Box and relays the response.
3. The client obtains a `<SID>` and starts issuing REST calls with
   `Authorization: AVM-SID <sid>`.
4. The proxy uses the normalised path as the sole cache key. All authenticated
   clients share the same cached responses because the Fritz!Box smart home API
   returns identical device-state data to every valid session. When a cache miss
   occurs, the proxy forwards the triggering client's SID in the upstream request;
   all other clients waiting on the same path receive the same response.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│  Client A  Client B  Client C  …                             │
│     │          │         │                                   │
│     └──────────┴─────────┘  HTTP/HTTPS (any Fritz!Box API calls)│
│                 │                                            │
│         ┌───────▼────────┐                                   │
│         │ HTTP/S Listener │  (ListenThread — one per port)   │
│         └───────┬────────┘                                   │
│                 │  spawns / posts to                         │
│         ┌───────▼────────┐                                   │
│         │  Worker Pool   │  (N threads, N = --threads arg)  │
│         └───────┬────────┘                                   │
│                 │                                            │
│    ┌────────────▼───────────────┐                            │
│    │       RequestRouter        │                            │
│    │  auth path → ForwardOnly   │                            │
│    │  PUT path  → ForwardOnly   │  + cache flush             │
│    │  GET path  → CacheLayer    │                            │
│    └──────┬────────────┬────────┘                            │
│           │            │                                     │
│    ┌──────▼──────┐  ┌──▼────────────────────────────────┐   │
│    │  ForwardOnly│  │  CacheLayer                        │   │
│    │  (passthru) │  │  cache hit → return immediately    │   │
│    └──────┬──────┘  │  in-flight → append callback,      │   │
│           │         │             wait on condvar         │   │
│           │         │  miss → ForwardOnly, store result   │   │
│           │         └────────────┬──────────────────────-─┘  │
│           │                      │                           │
│           └──────────┬───────────┘                           │
│                      │                                       │
│              ┌───────▼────────┐                              │
│              │  FritzClient   │  (HTTP client, one shared    │
│              │  (upstream)    │   QueuedRequest list, mutex) │
│              └───────┬────────┘                              │
│                      │  TCP/HTTPS                            │
│              ┌───────▼────────┐                              │
│              │   Fritz!Box    │                              │
│              └────────────────┘                              │
└──────────────────────────────────────────────────────────────┘
```

---

## Components

### 1. `HttpServer`

Accepts incoming TCP connections and dispatches each request to the worker pool.
Must handle HTTP/1.1 keep-alive connections. Headers, method, path, and body are
parsed minimally — only what is needed for routing.

Supports both plain HTTP and TLS (HTTPS):
- **HTTP mode** (default): no additional configuration required.
- **HTTPS/file mode**: activated when both `--tls-cert` and `--tls-key` are supplied.
  The server loads the PEM certificate and private key at startup and terminates
  TLS for all incoming connections. If either file is missing or unreadable the
  process exits with an error before binding.
- **HTTPS/self-signed mode**: activated by `--tls-self-signed` (no cert/key files
  required). An RSA-2048 private key and self-signed X.509 v3 certificate are
  generated in memory at startup using OpenSSL (`EVP_PKEY_keygen` + `X509_sign`),
  then passed to `httplib::SSLServer(X509*, EVP_PKEY*)`. The certificate is valid
  for 365 days. Clients will see a certificate warning unless they explicitly
  trust it. Cannot be combined with `--tls-cert`/`--tls-key`.

**Recommended library:** `cpp-httplib` (header-only, MIT, supports HTTP and HTTPS
via OpenSSL — use `httplib::SSLServer` when TLS files are provided, `httplib::Server`
otherwise).

### 2. `RequestRouter`

Classifies every incoming request:

| Rule | Action |
|---|---|
| Path starts with `/login_sid.lua` | `ForwardOnly` (never cache auth) |
| Method is `PUT`, `POST`, `DELETE`, or `PATCH` | `ForwardOnly` + flush cache entries for this SID |
| Method is `GET` and path starts with `/api/v0/smarthome/` | `CacheLayer` |
| Anything else | `ForwardOnly` |

### 3. `CacheLayer`

Thread-safe in-memory cache with:

- **Key:** normalised path only — query parameters sorted alphabetically, SID
  excluded. All authenticated clients share the same cache entries because the
  Fritz!Box smart home API returns identical device-state data to every valid
  session.
- **Value:** raw HTTP response body (bytes) + status code + response headers.
- **TTL:** per-entry, set from the `--ttl` argument at insertion time.
- **In-flight deduplication:** if an identical request is already in progress,
  new arrivals wait on a `std::condition_variable` rather than launching a
  second upstream request. The SID of the triggering request is used for the
  upstream call; all waiters receive the same response.
- **Eviction:** expired entries are removed lazily on lookup or by a background
  sweeper thread (wakes every `ttl/2` seconds, minimum 1 s).

Data structure sketch:

```cpp
struct CacheEntry {
    std::string           body;
    int                   status_code;
    Headers               headers;      // std::map<std::string, std::string>
    std::chrono::steady_clock::time_point expires_at;
    bool                  in_flight;    // true while upstream request is pending
    std::vector<std::function<void()>> waiters; // callbacks/condvar notify list
};

// protected by a single std::mutex (or std::shared_mutex for read-heavy loads)
std::unordered_map<std::string /*key*/, CacheEntry> m_cache;
std::condition_variable m_cv;
```

### 4. `FritzClient`

Thin HTTP client wrapper around the upstream Fritz!Box. Must:

- Forward all original request headers, including `Authorization`.
- Forward the original request body for PUT/POST.
- Return the raw response (status, headers, body).
- Support both HTTP and HTTPS to Fritz!Box (controlled by `--fritzbox-scheme`).

**Recommended library:** `cpp-httplib` client-side API or `libcurl`.

### 5. `Logger`

Writes structured trace lines to **stderr** only. Verbosity is controlled by a
single integer level (0 = silent, 1 = errors only, 2 = warnings, 3 = info,
4 = debug, 5 = trace). Thread-safe via a mutex or lock-free queue.

Format:

```
[<ISO-8601 timestamp>] [<LEVEL>] [<thread-id>] <message>
```

Example:

```
[2026-06-27T14:03:01.123] [DEBUG] [t3] CACHE HIT  sid=abc123 path=/api/v0/smarthome/overview ttl_remaining=3.7s
[2026-06-27T14:03:01.124] [TRACE] [t3] UPSTREAM    GET http://fritz.box/api/v0/smarthome/overview
[2026-06-27T14:03:01.456] [TRACE] [t3] UPSTREAM    200 OK 332 bytes in 332ms
```

---

## Command-Line Interface

```
fritzhome-cache [OPTIONS]

Options:
  -a, --fritzbox-addr <host[:port]>   Fritz!Box hostname or IP (default: fritz.box)
  -s, --fritzbox-scheme <http|https>  Upstream scheme (default: http)
  -p, --listen-port <port>            Port to listen on (default: 8080)
  -l, --listen-addr <addr>            Address to bind to (default: 127.0.0.1)
      --tls-cert <path>               Path to PEM certificate file (enables HTTPS listener)
      --tls-key <path>                Path to PEM private key file (enables HTTPS listener)
      --tls-self-signed               Generate in-memory self-signed cert at startup (enables HTTPS)
  -t, --ttl <seconds>                 Cache TTL for GET responses (default: 5)
      --ttl-overview <seconds>        TTL override for /overview endpoint
      --ttl-units <seconds>           TTL override for /overview/units/* endpoints
  -j, --threads <n>                   Worker thread count (default: 4)
      --sweep-interval <seconds>      Cache sweeper interval (default: ttl/2, min 1)
  -v, --verbose                       Increase log verbosity (repeat up to 5 times,
                                      e.g. -vvvvv for TRACE level)
      --log-level <0-5>               Set log level numerically (overrides -v count)
  -h, --help                          Print this help and exit
      --version                       Print version and exit
```

Level mapping for `-v` count:

| `-v` count | Level name | Numerical level |
|---|---|---|
| 0 (default) | ERROR | 1 |
| -v | WARN | 2 |
| -vv | INFO | 3 |
| -vvv | DEBUG | 4 |
| -vvvv | TRACE | 5 |
| -vvvvv | TRACE (same) | 5 |

---

## Concurrency Model

The application is multi-threaded. The following invariants must hold:

1. **One mutex per CacheLayer instance.** All cache reads and writes acquire this
   lock. Use `std::shared_mutex` if profiling shows lock contention; default to
   `std::mutex` for correctness first.

2. **In-flight deduplication without thundering-herd.** When a cache miss is
   detected, the worker sets `entry.in_flight = true` and releases the lock
   before making the upstream call. Other workers that arrive for the same key
   while `in_flight == true` block on a `std::condition_variable` (or
   `std::condition_variable_any` with `std::shared_mutex`). When the upstream
   response arrives the first worker stores the result, sets
   `in_flight = false`, and calls `m_cv.notify_all()`. Waiters then re-check
   the cache and return the stored response.

3. **No lock held during upstream I/O.** HTTP calls to Fritz!Box are always made
   outside any cache lock to avoid blocking other threads.

4. **PUT flush is atomic.** Before forwarding a PUT upstream, the worker
   acquires the cache lock, removes all entries for the matching path(s)
   (the unit's own entry and the overview entry), and releases the lock. The
   upstream call is then made without holding the lock.

5. **Logger mutex is separate.** The logging subsystem uses its own lock and
   must never be called while holding the cache lock (to avoid lock-order
   inversion).

---

## Error Handling

| Scenario | Behaviour |
|---|---|
| Fritz!Box returns HTTP 401 | Cache entry is not stored. Response forwarded to client as-is. Existing cached entries for the affected path are flushed. |
| Fritz!Box unreachable / timeout | Return `502 Bad Gateway` to client with a JSON error body `{"error": "upstream_unreachable"}`. Log at WARN level. |
| Cache entry expired mid-wait (edge case) | Treated as a miss; the waiting thread issues a fresh upstream request. |
| Worker thread throws | Catch at the top of the worker loop, log at ERROR, return `500 Internal Server Error` to client. Thread continues running. |
| TLS certificate or key file unreadable at startup | Log at ERROR and exit before binding. |
| TLS handshake failure (client side) | `cpp-httplib` closes the connection; log the OpenSSL error string at WARN level. No upstream request is made. |
| SIGTERM / SIGINT | Graceful shutdown: stop accepting new connections, wait for in-flight requests to complete (with a configurable `--shutdown-timeout`, default 5 s), then exit. |

---

## Data Flow: GET /api/v0/smarthome/overview (cache miss)

```
Client          Proxy (Worker T2)           CacheLayer         FritzClient       Fritz!Box
  │                    │                        │                   │                │
  │── GET overview ───►│                        │                   │                │
  │   Authorization:   │                        │                   │                │
  │   AVM-SID abc123   │                        │                   │                │
  │                    │── lookup(path) ────────►│                   │                │
  │                    │◄── MISS ───────────────│                   │                │
  │                    │── set in_flight=true ─►│                   │                │
  │                    │                        │                   │                │
  │                    │── GET overview (with Authorization header) ►                │
  │                    │                        │                   │── GET ────────►│
  │                    │                        │                   │◄── 200 body ───│
  │                    │◄── 200 body ───────────────────────────────│                │
  │                    │── store(entry, ttl) ──►│                   │                │
  │                    │── set in_flight=false,  │                  │                │
  │                    │   notify_all ──────────►│                  │                │
  │◄── 200 body ───────│                        │                   │                │
```
Client          Proxy (Worker T2)           CacheLayer         FritzClient       Fritz!Box
  │                    │                        │                   │                │
  │── GET overview ───►│                        │                   │                │
  │   Authorization:   │                        │                   │                │
  │   AVM-SID abc123   │                        │                   │                │
  │                    │── lookup(sid, path) ──►│                   │                │
  │                    │◄── MISS ───────────────│                   │                │
  │                    │── set in_flight=true ─►│                   │                │
  │                    │                        │                   │                │
  │                    │── GET overview (with Authorization header) ►                │  │                    │                        │                   │── GET ────────►│
  │                    │                        │                   │◄── 200 body ───│
  │                    │◄── 200 body ───────────────────────────────│                │
  │                    │── store(entry, ttl) ──►│                   │                │
  │                    │── set in_flight=false,  │                  │                │
  │                    │   notify_all ──────────►│                  │                │
  │◄── 200 body ───────│                        │                   │                │
```

---

## Data Flow: GET /api/v0/smarthome/overview (concurrent, in-flight dedup)

```
Client A        Worker T2          CacheLayer        FritzClient     Fritz!Box
  │                 │                   │                  │              │
  │── GET ─────────►│── lookup ────────►│                  │              │
  │                 │◄── MISS ──────────│                  │              │
  │                 │── in_flight=true ►│                  │              │
  │                 │                   │                  │── GET ──────►│
  │                 │                   │                  │              │ (pending)

Client B        Worker T3          CacheLayer
  │                 │                   │
  │── GET ─────────►│── lookup ────────►│
  │                 │◄── in_flight=true-│
  │                 │── wait(cv) ───────────────────────────────────────────
  │                 │   (blocked)       │
                                        │              │◄── 200 body ────│
  T2             ◄─────────────────────────────────────│
  │── store, notify_all ──────────────►│
  T3 wakes        │◄── HIT ────────────│
  │◄── 200 body ──│                    │
Client A◄─ 200 ──T2
Client B◄─ 200 ──T3
```

---

## Technology Stack

| Concern | Recommended Library | Notes |
|---|---|---|
| HTTP server + client | [`cpp-httplib`](https://github.com/yhirose/cpp-httplib) | Header-only, HTTP/1.1, HTTPS via OpenSSL |
| JSON parsing | [`nlohmann/json`](https://github.com/nlohmann/json) | Header-only, MIT |
| Crypto (PBKDF2, MD5) | **OpenSSL** (`libssl`, `libcrypto`) | Required by cpp-httplib for HTTPS anyway |
| Threading primitives | C++17 STL (`std::thread`, `std::mutex`, `std::shared_mutex`, `std::condition_variable`) | |
| CLI argument parsing | [`CLI11`](https://github.com/CLIUtils/CLI11) | Header-only, MIT |
| Build system | **CMake** ≥ 3.20 | |
| Minimum C++ standard | **C++17** | |

---

## Key Source Files (Planned Layout)

```
fritzhome-cache/
├── src/
│   ├── main.cpp               CLI parsing, startup, graceful shutdown
│   ├── config.h               Config struct populated from CLI args
│   ├── logger.h / logger.cpp  Thread-safe stderr logger with verbosity levels
│   ├── cache.h / cache.cpp    CacheLayer: TTL store, dedup, sweeper thread
│   ├── router.h / router.cpp  RequestRouter: classifies requests
│   ├── fritzClient.h / fritzClient.cpp  Upstream HTTP client wrapper
│   └── server.h / server.cpp  HttpServer: listener + worker-pool integration
├── CMakeLists.txt
├── AGENTS.md
└── README.md
```

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Dependencies resolved via CMake's `FetchContent` or the system package manager:

```cmake
find_package(OpenSSL REQUIRED)
FetchContent_Declare(httplib  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib GIT_TAG v0.18.0)
FetchContent_Declare(nlohmann GIT_REPOSITORY https://github.com/nlohmann/json       GIT_TAG v3.11.3)
FetchContent_Declare(CLI11    GIT_REPOSITORY https://github.com/CLIUtils/CLI11      GIT_TAG v2.4.2)
```

---

## Testing

- **Unit tests:** Google Test (`GTest`) — test `CacheLayer`, `RequestRouter`,
  and `Logger` in isolation with mock upstreams.
- **Integration tests:** A small Python script starts the proxy, issues
  concurrent requests via `httpx`, and asserts cache hit/miss metrics returned
  in a `/status` endpoint (optional, debug builds only).
- **Load test:** `wrk` or `hey` against the running proxy pointing at a
  Fritz!Box mock (or real device) to verify dedup behaviour under concurrency.

---

## Coding Guidelines

- Prefer `std::string_view` for read-only string parameters.
- Use `[[nodiscard]]` on all functions returning error codes or cache results.
- No raw `new`/`delete`; use `std::unique_ptr` and `std::shared_ptr`.
- All public API functions are documented with a one-line comment explaining
  thread-safety guarantees (e.g. `// thread-safe; acquires m_mutex`).
- Log every cache decision at DEBUG level and every upstream request/response at
  TRACE level (see Logger section).
- Do not log passwords, SIDs, or any credential material at any verbosity level.

---

## Security Notes

- The proxy binds to `127.0.0.1` by default. Binding to `0.0.0.0` requires the
  `--listen-addr 0.0.0.0` flag and is the operator's responsibility to secure.
- **HTTPS listener:** supply `--tls-cert` and `--tls-key` (PEM format) to enable
  TLS termination on the listening side. Both flags must be provided together; if
  only one is given the process exits with an error before binding. When neither
  is provided the server listens on plain HTTP unless `--tls-self-signed` is given.
- **HTTPS self-signed:** `--tls-self-signed` generates an RSA-2048, SHA-256
  self-signed certificate in memory at startup. Mutually exclusive with
  `--tls-cert`/`--tls-key`; combining them is an error. Clients must explicitly
  trust the certificate to avoid browser/curl warnings.
- SIDs are treated as opaque tokens. They appear in log output only at TRACE
  level and only as a truncated prefix (first 4 characters + `…`).
- The proxy does not validate TLS certificates of the Fritz!Box by default (many
  routers use self-signed certs). Add a `--verify-tls` flag to opt in to
  strict verification.
