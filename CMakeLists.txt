cmake_minimum_required(VERSION 3.20)
project(fritzhome-cache VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Options ───────────────────────────────────────────────────────────────────

# When ON, link OpenSSL and the C++ runtime statically; libc stays shared so
# DNS resolution and NSS still work at runtime.  Requires that the build image
# has static libraries available (all recommended images already do).
option(FRITZHOME_CACHE_STATIC
    "Link OpenSSL and C++ runtime statically; libc stays shared" OFF)

# cpp-httplib git tag.  Defaults to the latest tested release.  Set to an older
# tag when building against OpenSSL 1.1.1 (e.g. Raspberry Pi OS bullseye):
#   -DFRITZHOME_HTTPLIB_TAG=v0.14.3
# Background: v0.15.0+ sets _HTTPLIB_OPENSSL_MIN_VER to "3.0.0" so it refuses
# to compile against OpenSSL 1.1.1; v0.14.3 still allows "1.1.1".
set(FRITZHOME_HTTPLIB_TAG "v0.18.0" CACHE STRING "cpp-httplib git tag to fetch")

# ── Dependencies ──────────────────────────────────────────────────────────────

# Must be set BEFORE find_package(OpenSSL) so FindOpenSSL.cmake honours it.
if(FRITZHOME_CACHE_STATIC)
    set(OPENSSL_USE_STATIC_LIBS ON)
endif()

find_package(OpenSSL REQUIRED)

include(FetchContent)

# Tell cpp-httplib to require OpenSSL so its CMake target carries it as a dep
set(HTTPLIB_REQUIRE_OPENSSL ON  CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib
    GIT_TAG        ${FRITZHOME_HTTPLIB_TAG}
)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG        v3.11.3
)
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11
    GIT_TAG        v2.4.2
)

FetchContent_MakeAvailable(httplib nlohmann_json CLI11)

# ── Target ────────────────────────────────────────────────────────────────────

add_executable(fritzhome-cache
    src/main.cpp
    src/logger.cpp
    src/cache.cpp
    src/fritzClient.cpp
    src/router.cpp
    src/server.cpp
)

target_include_directories(fritzhome-cache PRIVATE src)

# CPPHTTPLIB_OPENSSL_SUPPORT must be defined before including httplib.h
target_compile_definitions(fritzhome-cache PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_link_libraries(fritzhome-cache PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    OpenSSL::SSL
    OpenSSL::Crypto
)

# Threads (required on Linux)
find_package(Threads REQUIRED)
target_link_libraries(fritzhome-cache PRIVATE Threads::Threads)

# CMake's cmake_have_libc_pthread check runs against the BUILD host, not the
# cross-compile target.  On Debian 11 (bullseye) cross-compile containers the
# host check passes (amd64 glibc 2.31 has pthread in libc from the cmake
# perspective), so Threads::Threads adds no -lpthread.  The target armhf libc
# 2.31 still ships pthreads as a separate libpthread.so.0, and libssl/libcrypto
# reference pthread_create.  Adding -lpthread explicitly when cross-compiling
# ensures the symbol is always resolved; on glibc >= 2.34 -lpthread is a stub
# so this is harmless for bookworm and newer targets.
if(CMAKE_CROSSCOMPILING)
    target_link_libraries(fritzhome-cache PRIVATE pthread)
endif()

# Static C++ runtime + OpenSSL (libc intentionally stays shared).
# ${CMAKE_DL_LIBS} (-ldl) is needed because OpenSSL's provider/engine code
# uses dlopen() at runtime even when linked from a static archive.
if(FRITZHOME_CACHE_STATIC)
    target_link_options(fritzhome-cache PRIVATE -static-libstdc++ -static-libgcc)
    target_link_libraries(fritzhome-cache PRIVATE ${CMAKE_DL_LIBS})
endif()

# ── Install ───────────────────────────────────────────────────────────────────

install(TARGETS fritzhome-cache RUNTIME DESTINATION bin)

# systemd unit file → /usr/lib/systemd/system/fritzhome-cache.service
install(FILES data/fritzhome-cache.service
    DESTINATION lib/systemd/system)

# Configuration file → /etc/fritzhome-cache/fritzhome-cache.conf
# Absolute destination so the path is not prefixed with CPACK_PACKAGING_INSTALL_PREFIX.
install(FILES data/fritzhome-cache.conf
    DESTINATION /etc/fritzhome-cache)

# ── Packaging (CPack) ─────────────────────────────────────────────────────────

# Install prefix for packaged files: /usr (so "bin" → /usr/bin,
# "lib/systemd/system" → /usr/lib/systemd/system, etc.)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

set(CPACK_PACKAGE_NAME        "fritzhome-cache")
set(CPACK_PACKAGE_VENDOR      "Ulf Saran")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Transparent HTTP/HTTPS reverse-proxy and cache for the Fritz!Box Smart Home API")
set(CPACK_PACKAGE_VERSION     "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT     "Ulf Saran")

# RPM (openSUSE)
set(CPACK_RPM_PACKAGE_RELEASE      "1")
set(CPACK_RPM_PACKAGE_LICENSE      "MIT")
set(CPACK_RPM_PACKAGE_GROUP        "System/Daemons")
set(CPACK_RPM_PACKAGE_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
# Dynamic build depends on the shared OpenSSL library; static build ships its own.
if(FRITZHOME_CACHE_STATIC)
    set(CPACK_RPM_PACKAGE_REQUIRES "")
else()
    set(CPACK_RPM_PACKAGE_REQUIRES "libopenssl3")
endif()
# RPM scriptlets — reload systemd on install/upgrade, stop and disable on removal
set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE   "${CMAKE_SOURCE_DIR}/data/rpm/postinst.sh")
set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE  "${CMAKE_SOURCE_DIR}/data/rpm/preun.sh")
set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE "${CMAKE_SOURCE_DIR}/data/rpm/postun.sh")
# Mark the config file as %config(noreplace) so rpm does not overwrite user
# edits on upgrade.  Exclude it from CPack's auto-generated %files list first,
# then re-declare it (with %dir ownership of the directory) via USER_FILELIST.
# Note: /etc is already in CPack's default CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST.
list(APPEND CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST
    "/etc/fritzhome-cache"
    "/etc/fritzhome-cache/fritzhome-cache.conf")
set(CPACK_RPM_USER_FILELIST
    "%dir /etc/fritzhome-cache"
    "%config(noreplace) /etc/fritzhome-cache/fritzhome-cache.conf")

# DEB (Debian / Raspbian)
set(CPACK_DEBIAN_PACKAGE_RELEASE    "1")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Ulf Saran")
# Dynamic build depends on the shared OpenSSL library; static build ships its own.
# Auto-detect the package name from the OpenSSL version found at configure time:
#   OpenSSL 1.x  →  libssl1.1   (e.g. Debian/Raspberry Pi OS bullseye)
#   OpenSSL 3.x  →  libssl3     (e.g. Debian/Raspberry Pi OS bookworm)
if(FRITZHOME_CACHE_STATIC)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "")
elseif(OpenSSL_VERSION VERSION_GREATER_EQUAL "3.0.0")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libssl3")
else()
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libssl1.1")
endif()
set(CPACK_DEBIAN_PACKAGE_SECTION    "net")
# Map CMake's processor name to Debian's architecture vocabulary.
# CPack defaults to the *host* architecture when this is not set, which is
# wrong for cross-compiled builds (e.g. aarch64 cross-compiled on x86_64
# would otherwise produce a package labelled "amd64").
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "armhf")
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
else()
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
endif()
# DEB control scripts — reload systemd on install/upgrade, stop and disable on removal
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_SOURCE_DIR}/data/debian/postinst"
    "${CMAKE_SOURCE_DIR}/data/debian/prerm"
    "${CMAKE_SOURCE_DIR}/data/debian/postrm"
    "${CMAKE_SOURCE_DIR}/data/debian/conffiles")

include(CPack)
