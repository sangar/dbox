#!/bin/sh
if command -v systemctl >/dev/null 2>&1; then
    systemctl --global disable dbox.service >/dev/null 2>&1 || true
fi
