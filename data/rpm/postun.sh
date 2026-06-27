#!/bin/sh
# RPM %postun — runs after the package is removed or upgraded.
# $1 = 0 on final removal, $1 >= 1 on upgrade.
set -e
systemctl daemon-reload || true
