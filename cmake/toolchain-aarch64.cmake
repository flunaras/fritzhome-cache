# cmake/toolchain-aarch64.cmake
#
# CMake cross-compilation toolchain for aarch64-suse-linux.
#
# Used by docker/Dockerfile.tumbleweed-aarch64 together with the
# cross-aarch64-gcc14 package from the standard Tumbleweed OSS repo.
#
# The cross-aarch64-gcc14 package ships a minimal glibc sysroot at
# /usr/aarch64-suse-linux/sys-root (crt1.o, libc, libgcc_s …).
# The Dockerfile installs OpenSSL aarch64 headers and .so stubs on top of that
# sysroot (extracted from the openSUSE ports mirror).
#
# Pass to cmake:
#   cmake -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchain-aarch64.cmake ...

# ── Target system description ─────────────────────────────────────────────────
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# ── Cross-compiler (installed by cross-aarch64-gcc14 package) ─────────────────
set(CMAKE_C_COMPILER   /usr/bin/aarch64-suse-linux-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/aarch64-suse-linux-g++)
set(CMAKE_AR           /usr/bin/aarch64-suse-linux-ar)
set(CMAKE_RANLIB       /usr/bin/aarch64-suse-linux-ranlib)
set(CMAKE_STRIP        /usr/bin/aarch64-suse-linux-strip)

# ── Sysroot ───────────────────────────────────────────────────────────────────
# cross-aarch64-gcc14 ships glibc crt objects + stubs at:
#   /usr/aarch64-suse-linux/sys-root/
# The Dockerfile extracts OpenSSL aarch64 RPMs (from the openSUSE ports mirror)
# into that same sysroot, providing headers and .so symlinks under:
#   /usr/aarch64-suse-linux/sys-root/usr/
set(CMAKE_SYSROOT /usr/aarch64-suse-linux/sys-root)

# Tell the linker where to look for aarch64 shared libraries.
# --allow-shlib-undefined: suppress "undefined reference in .so" errors for
# transitive deps (libz, libdl, …) not present in the sysroot; they are
# available at runtime on the target system.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-Wl,-rpath-link,/usr/aarch64-suse-linux/sys-root/usr/lib64 -Wl,--allow-shlib-undefined")
set(CMAKE_SHARED_LINKER_FLAGS_INIT
    "-Wl,-rpath-link,/usr/aarch64-suse-linux/sys-root/usr/lib64 -Wl,--allow-shlib-undefined")

# ── Find-root-path settings ───────────────────────────────────────────────────
# Search the sysroot for headers, libraries, and packages.
# Use the host for executables (cmake, ninja, git, …).
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ── Cross-compilation: try-compile workaround ────────────────────────────────
# Prevents CMake from trying to link an aarch64 executable on an x86_64 host
# (no QEMU required).  Feature checks compile to a static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# ── OpenSSL location hint ─────────────────────────────────────────────────────
# Steer find_package(OpenSSL) into the sysroot so it does not pick up the
# host's x86_64 OpenSSL installation.
set(OPENSSL_ROOT_DIR "${CMAKE_SYSROOT}/usr" CACHE PATH "OpenSSL root for aarch64 cross-compile" FORCE)
