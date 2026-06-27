#!/bin/sh
# RPM %preun — runs before the package is removed or upgraded.
# $1 = 0 on final removal, $1 >= 1 on upgrade (keep service running).
set -e
if [ "$1" -eq 0 ]; then
    if systemctl is-active --quiet fritzhome-cache.service 2>/dev/null; then
        systemctl stop fritzhome-cache.service || true
    fi
    if systemctl is-enabled --quiet fritzhome-cache.service 2>/dev/null; then
        systemctl disable fritzhome-cache.service || true
    fi
fi
