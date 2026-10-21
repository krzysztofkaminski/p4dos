#!/usr/bin/env bash
# Build the P4DOS firmware (ESP-IDF 5.5.x, target esp32p4).
#
#   ./build.sh                 build firmware; also builds the FreeDOS disk image if missing
#   ./build.sh flash monitor -p /dev/ttyACM0     any other idf.py command line
#
# For boards with chip revision v3.x, set P4DOS_REV3=1: that build has its own directory
# (build-rev3) and sdkconfig (sdkconfig.rev3), with sdkconfig.defaults.rev3 on top.
#
# ESP-IDF and its tools are the project's own copy in toolchain/ (esp-idf-5.5.4, and
# espressif/ as IDF_TOOLS_PATH), not one another project uses; without it, IDF_PATH must be
# exported.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

if [ -f "$ROOT/toolchain/esp-idf-5.5.4/export.sh" ]; then
    export IDF_PATH="$ROOT/toolchain/esp-idf-5.5.4"
    export IDF_TOOLS_PATH="$ROOT/toolchain/espressif"
    unset IDF_PYTHON_ENV_PATH
fi
if [ -z "${IDF_PATH:-}" ] || [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ESP-IDF not found: set IDF_PATH to an ESP-IDF 5.5 checkout" >&2
    exit 2
fi

if [ ! -f "$ROOT/build/p4dos-hd.img" ]; then
    make -C "$ROOT" image
fi

# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null

B="$HERE/build"
CFG=()
if [ "${P4DOS_REV3:-0}" = 1 ]; then
    B="$HERE/build-rev3"
    CFG=(-D "SDKCONFIG=$HERE/sdkconfig.rev3" -D "SDKCONFIG_DEFAULTS=$HERE/sdkconfig.defaults;$HERE/sdkconfig.defaults.rev3")
fi
if [ ! -f "$B/CMakeCache.txt" ]; then
    idf.py -C "$HERE" -B "$B" ${CFG[@]+"${CFG[@]}"} set-target esp32p4
fi
if [ "$#" -eq 0 ]; then
    set -- build
fi
exec idf.py -C "$HERE" -B "$B" ${CFG[@]+"${CFG[@]}"} "$@"
