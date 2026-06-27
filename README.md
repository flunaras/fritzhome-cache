# fritzhome-cache

A transparent HTTP/HTTPS reverse-proxy and cache that sits between smart-home
clients and a Fritz!Box router. It reduces load on the Fritz!Box by serving
cached responses for read-only API calls while passing write commands and
authentication requests through unchanged.

- **Transparent** — clients use the same Fritz!Box API unchanged; only the
  host address needs to point to the proxy.
- **No stored credentials** — session IDs are never persisted; every Fritz!Box
  session uses credentials supplied by the connecting client.
- **Concurrent** — multiple clients are served simultaneously; in-flight
  requests for the same path are deduplicated rather than fanned out to the
  upstream router.
- **Written in C++17**, built with CMake, packaged as RPM (openSUSE) and DEB
  (Debian).

---

## Building

Builds run inside Docker containers so no local toolchain is required.

```bash
# Build for all targets (openSUSE Tumbleweed x86_64 + aarch64, Debian 12 amd64,
#                        Raspberry Pi OS bookworm aarch64 + armhf,
#                        Raspberry Pi OS bullseye armhf)
bash docker/build.sh --distro all

# Build for a single target
bash docker/build.sh --distro opensuse-tumbleweed-x86_64
bash docker/build.sh --distro opensuse-tumbleweed-aarch64
bash docker/build.sh --distro debian-12-x86_64
bash docker/build.sh --distro raspbian-bookworm-aarch64    # 64-bit Raspberry Pi OS (bookworm)
bash docker/build.sh --distro raspbian-bookworm-armhf      # 32-bit Raspberry Pi OS (bookworm)
bash docker/build.sh --distro raspbian-bullseye-armhf      # 32-bit Raspberry Pi OS (bullseye, older Pi)

# Debug build
bash docker/build.sh --distro debian-12-x86_64 --build-type Debug

# Static linking (OpenSSL + C++ runtime; libc stays dynamic)
bash docker/build.sh --distro opensuse-tumbleweed-x86_64 --static
```

Output lands in `out/<family>/<distro>/<arch>/`:

```
out/
├── opensuse/tumbleweed/x86_64/
│   ├── fritzhome-cache
│   └── fritzhome-cache-1.0.0-1.x86_64.rpm
├── opensuse/tumbleweed/aarch64/
│   ├── fritzhome-cache
│   └── fritzhome-cache-1.0.0-1.aarch64.rpm
├── debian/12/amd64/
│   ├── fritzhome-cache
│   └── fritzhome-cache_1.0.0-1_amd64.deb
├── raspbian/bookworm/arm64/
│   ├── fritzhome-cache
│   └── fritzhome-cache_1.0.0-1_arm64.deb
├── raspbian/bookworm/armhf/
│   ├── fritzhome-cache
│   └── fritzhome-cache_1.0.0-1_armhf.deb
└── raspbian/bullseye/armhf/
    ├── fritzhome-cache                  (glibc 2.31 — runs on older Pi OS installs)
    └── fritzhome-cache_1.0.0-1_armhf.deb
```

### Prerequisites

- Docker (tested with Docker CE 29)
- Internet access for the first build (base images and FetchContent dependencies
  are cached by Docker on subsequent runs)

### Manual build (host toolchain)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Requires: CMake ≥ 3.20, a C++17-capable compiler, OpenSSL ≥ 1.1.1 development
headers, and internet access for FetchContent (cpp-httplib, nlohmann/json,
CLI11).

> **Note:** With OpenSSL 1.1.1 (e.g. Debian/Ubuntu 20.04 / Raspberry Pi OS
> bullseye), pass `-DFRITZHOME_HTTPLIB_TAG=v0.14.3` to cmake so cpp-httplib is
> fetched at a version compatible with OpenSSL 1.1.1.  The Docker builds handle
> this automatically.

---

## Installation

### RPM (openSUSE)

```bash
sudo zypper install ./fritzhome-cache-1.0.0-1.x86_64.rpm
```

### DEB (Debian / Ubuntu)

```bash
sudo apt install ./fritzhome-cache_1.0.0-1_amd64.deb
```

Both packages install:

| Path | Contents |
|---|---|
| `/usr/bin/fritzhome-cache` | Executable |
| `/etc/fritzhome-cache/fritzhome-cache.conf` | Configuration file |
| `/usr/lib/systemd/system/fritzhome-cache.service` | systemd unit |

---

## Running

### Direct

```bash
# Minimal — proxy fritz.box, listen on 127.0.0.1:8080, 5 s TTL
fritzhome-cache

# Custom upstream and listener
fritzhome-cache --fritzbox-addr 192.168.178.1 --listen-port 9090 --ttl 10

# Verbose output
fritzhome-cache -vvv          # INFO level
fritzhome-cache -vvvv         # DEBUG level
```

### systemd

```bash
# Edit /etc/fritzhome-cache/fritzhome-cache.conf first, then:
sudo systemctl enable --now fritzhome-cache
sudo journalctl -u fritzhome-cache -f
```

---

## Configuration

All options can be set in `/etc/fritzhome-cache/fritzhome-cache.conf` (CLI11
INI format). Command-line arguments always override the config file.

```ini
# Fritz!Box address and upstream scheme
# fritzbox-addr = fritz.box
# fritzbox-scheme = http

# Listener
# listen-addr = 127.0.0.1
# listen-port = 8080

# Cache TTLs (seconds)
# ttl = 5
# ttl-overview =
# ttl-units =

# Worker threads
# threads = 4

# Log level: 0=silent 1=ERROR 2=WARN 3=INFO 4=DEBUG 5=TRACE
# log-level = 1
```

A fully commented example is installed at
`/etc/fritzhome-cache/fritzhome-cache.conf`.

---

## Command-line reference

```
fritzhome-cache [OPTIONS]

Options:
  -a, --fritzbox-addr <host[:port]>   Fritz!Box hostname or IP  (default: fritz.box)
  -s, --fritzbox-scheme <http|https>  Upstream scheme            (default: http)
  -p, --listen-port <port>            Port to listen on          (default: 8080)
  -l, --listen-addr <addr>            Address to bind to         (default: 127.0.0.1)
      --tls-cert <path>               PEM certificate — enables HTTPS listener
      --tls-key  <path>               PEM private key  — enables HTTPS listener
      --tls-self-signed               Generate in-memory self-signed cert (enables HTTPS)
  -t, --ttl <seconds>                 Default cache TTL          (default: 5)
      --ttl-overview <seconds>        TTL override for /overview
      --ttl-units <seconds>           TTL override for /overview/units/*
  -j, --threads <n>                   Worker thread count        (default: 4)
      --sweep-interval <seconds>      Sweeper interval           (default: max(1, ttl/2))
      --verify-tls                    Verify Fritz!Box TLS certificate
      --shutdown-timeout <seconds>    Graceful shutdown timeout  (default: 5)
  -v, --verbose                       Increase log verbosity (repeat up to 5×)
      --log-level <0-5>               Set log level numerically (overrides -v)
      --config <path>                 Config file path
  -h, --help                          Print help and exit
      --version                       Print version and exit
```

Log levels: `0` = silent, `1` = ERROR (default), `2` = WARN, `3` = INFO,
`4` = DEBUG, `5` = TRACE.

---

## TLS modes

| Mode | Flags | Notes |
|---|---|---|
| Plain HTTP | *(none)* | Default |
| HTTPS with certificate | `--tls-cert cert.pem --tls-key key.pem` | Both flags required together |
| HTTPS self-signed | `--tls-self-signed` | RSA-2048 cert generated in memory at startup; clients will see a certificate warning |

`--tls-self-signed` and `--tls-cert`/`--tls-key` are mutually exclusive.

The proxy does **not** verify the Fritz!Box's TLS certificate by default (most
routers use self-signed certs). Pass `--verify-tls` to enable strict
verification.

---

## Caching behaviour

| Request | Action |
|---|---|
| `GET /api/v0/smarthome/overview` | Cached (shared across all authenticated clients) |
| `GET /api/v0/smarthome/overview/units/<uid>` | Cached |
| `PUT /api/v0/smarthome/overview/units/<uid>` | Forwarded; overview cache flushed |
| `POST`, `DELETE`, `PATCH` | Forwarded; cache flushed |
| `/login_sid.lua` (any method) | Always forwarded, never cached |
| Everything else | Forwarded, never cached |

Concurrent requests for the same path are deduplicated: only one upstream
request is issued; all other waiting clients receive the same response when
it arrives.

---

## Security notes

- Binds to `127.0.0.1` by default. Use `--listen-addr 0.0.0.0` to expose on
  all interfaces — secure the port externally when doing so.
- SIDs appear in log output only at TRACE level, truncated to the first four
  characters.
- Passwords are never logged at any verbosity level.
