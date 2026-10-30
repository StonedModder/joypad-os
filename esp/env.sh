#!/usr/bin/env bash
# Activates ESP-IDF for the ESP32-S3 builds.
#
# Changed from upstream for portability (upstream assumes Linux/macOS and
# $HOME/esp-idf):
#   * honour an already-set IDF_PATH;
#   * do not pin a python env that may not exist here;
#   * if the environment is already prepared (idf.py on PATH), do nothing.
if command -v idf.py >/dev/null 2>&1; then
    return 0 2>/dev/null || exit 0
fi

if [ -z "$IDF_PATH" ]; then
    export IDF_PATH="$HOME/esp-idf"
fi

if [ -f "$IDF_PATH/export.sh" ]; then
    # shellcheck disable=SC1090
    . "$IDF_PATH/export.sh" >/dev/null 2>&1
fi
