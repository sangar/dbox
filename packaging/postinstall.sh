#!/bin/sh
set -e
if command -v sysctl >/dev/null 2>&1; then
    sysctl --system >/dev/null 2>&1 || true
fi
echo "Write ~/.config/dbox/config.yml, then keep dbox running from login with:"
echo "  dbox service enable"
