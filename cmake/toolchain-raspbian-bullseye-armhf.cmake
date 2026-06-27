# cmake/toolchain-raspbian-bullseye-armhf.cmake
#
# CMake cross-compilation toolchain for Raspberry Pi OS bullseye (armhf, 32-bit).
#
# This toolchain targets Raspberry Pi OS bullseye (Debian 11, glibc 2.31,
# OpenSSL 1.1.1) and is used together with Dockerfile.raspbian-bullseye-armhf.
#
# Building on a debian:11 base ensures the binary only uses glibc symbols
# available in glibc 2.31, making it compatible with bullseye installations.
# (A bookworm-compiled binary requires glibc 2.36 and will fail on bullseye
# with "GLIBC_2.3x not found".)
#
# Because bullseye ships OpenSSL 1.1.1, cpp-httplib must be pinned to v0.14.3:
#   -DFRITZHOME_HTTPLIB_TAG=v0.14.3
# (v0.15.0+ refuses to compile against OpenSSL < 3.0.0)
#
# Toolchain settings are otherwise identical to toolchain-raspbian-armhf.cmake
# (same cross-compiler prefix, same OpenSSL staging root layout).
#
# Pass to cmake:
#   cmake -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-raspbian-bullseye-armhf.cmake ...

# ── Target system description ─────────────────────────────────────────────────
set(CMAKE_SYSTEM_NAME      Linux)
# armv7l is what Raspberry Pi OS 32-bit reports via `uname -m`.
# CMakeLists.txt maps any CMAKE_SYSTEM_PROCESSOR starting with "arm" to the
# Debian architecture name "armhf" for CPACK_DEBIAN_PACKAGE_ARCHITECTURE.
set(CMAKE_SYSTEM_PROCESSOR armv7l)

# ── Cross-compiler (installed by g++-arm-linux-gnueabihf package) ─────────────
set(CMAKE_C_COMPILER   /usr/bin/arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/arm-linux-gnueabihf-g++)
set(CMAKE_AR           /usr/bin/arm-linux-gnueabihf-ar      CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB       /usr/bin/arm-linux-gnueabihf-ranlib   CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP        /usr/bin/arm-linux-gnueabihf-strip    CACHE FILEPATH "" FORCE)

# ── Find-root-path settings ───────────────────────────────────────────────────
# The Debian multiarch cross-compiler already knows to search
# /usr/lib/arm-linux-gnueabihf/ and /usr/include/arm-linux-gnueabihf/ for
# target headers and libraries, so no CMAKE_SYSROOT is needed.
# Restrict CMake's find_* calls to the target paths so it cannot accidentally
# pick up host (x86_64) libraries.
set(CMAKE_FIND_ROOT_PATH /usr/lib/arm-linux-gnueabihf /usr/include)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ── Cross-compilation: try-compile workaround ─────────────────────────────────
# Prevents CMake from trying to run an armhf executable on an x86_64 host
# (no QEMU required). Feature checks compile to a static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ── OpenSSL location hint ─────────────────────────────────────────────────────
# Steer find_package(OpenSSL) at the staging root created by the Dockerfile:
#   /opt/openssl-armhf/include → /usr/include
#   /opt/openssl-armhf/lib     → /usr/lib/arm-linux-gnueabihf
set(OPENSSL_ROOT_DIR "/opt/openssl-armhf" CACHE PATH "OpenSSL root for Raspbian armhf cross-compile" FORCE)
