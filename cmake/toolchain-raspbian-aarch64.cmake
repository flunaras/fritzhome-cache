# cmake/toolchain-raspbian-aarch64.cmake
#
# CMake cross-compilation toolchain for Raspberry Pi OS bookworm (aarch64).
#
# Used by docker/Dockerfile.raspbian-aarch64 together with Debian's multiarch
# cross-compilation support:
#
#   g++-aarch64-linux-gnu    — cross-compiler installed from the Debian repo
#   libssl-dev:arm64         — OpenSSL headers + arm64 .so/.a via dpkg multiarch
#
# The Dockerfile creates a staging root at /opt/openssl-aarch64:
#   include/ → /usr/include              (OpenSSL headers, shared across arches)
#   lib/     → /usr/lib/aarch64-linux-gnu (arm64 libssl.{so,a} + libcrypto.{so,a})
#
# Pass to cmake:
#   cmake -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-raspbian-aarch64.cmake ...

# ── Target system description ─────────────────────────────────────────────────
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# ── Cross-compiler (installed by g++-aarch64-linux-gnu package) ───────────────
set(CMAKE_C_COMPILER   /usr/bin/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/aarch64-linux-gnu-g++)
set(CMAKE_AR           /usr/bin/aarch64-linux-gnu-ar     CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       /usr/bin/aarch64-linux-gnu-ranlib  CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP        /usr/bin/aarch64-linux-gnu-strip   CACHE FILEPATH "" FORCE)

# ── Find-root-path settings ───────────────────────────────────────────────────
# The Debian multiarch cross-compiler already knows to search
# /usr/lib/aarch64-linux-gnu/ and /usr/include/aarch64-linux-gnu/ for target
# headers and libraries, so no CMAKE_SYSROOT is needed.
# Restrict CMake's find_* calls to the target paths so it cannot accidentally
# pick up host (x86_64) libraries.
set(CMAKE_FIND_ROOT_PATH /usr/lib/aarch64-linux-gnu /usr/include)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ── Cross-compilation: try-compile workaround ─────────────────────────────────
# Prevents CMake from trying to run an aarch64 executable on an x86_64 host
# (no QEMU required). Feature checks compile to a static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ── OpenSSL location hint ─────────────────────────────────────────────────────
# Steer find_package(OpenSSL) at the staging root created by the Dockerfile:
#   /opt/openssl-aarch64/include → /usr/include
#   /opt/openssl-aarch64/lib     → /usr/lib/aarch64-linux-gnu
#
# FindOpenSSL.cmake searches OPENSSL_ROOT_DIR/include/ for headers and
# OPENSSL_ROOT_DIR/lib/ for libssl.{so,a} / libcrypto.{so,a}.
# When OPENSSL_USE_STATIC_LIBS is set (--static build), it prefers .a files.
set(OPENSSL_ROOT_DIR "/opt/openssl-aarch64" CACHE PATH "OpenSSL root for Raspbian aarch64 cross-compile" FORCE)
