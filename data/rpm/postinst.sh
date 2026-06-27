#!/bin/sh
# RPM %post — runs after the package is installed or upgraded.
# $1 = 1 on first install, $1 >= 2 on upgrade.
set -e
if [ "$1" -ge 1 ]; then
    systemctl daemon-reload || true
fi
