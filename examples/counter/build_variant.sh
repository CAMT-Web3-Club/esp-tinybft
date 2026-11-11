#!/usr/bin/env bash
# Usage: ./build_variant.sh <espnow|udp_psnone|udp_psmin|udp_psmax> <replica|client> [build|flash|...]
# Builds into build_<variant>_<role>/ with its own sdkconfig, from overlays only.
set -e
V=${1:?variant}; R=${2:?role}; shift 2
D="sdkconfig.defaults;sdkconfig.variant.$V;sdkconfig.role.$R;sdkconfig.measure"
[ -f sdkconfig.wifi.local ] && D="$D;sdkconfig.wifi.local"
B="build_${V}_${R}"
exec idf.py -B "$B" -D "SDKCONFIG=$B/sdkconfig" -D "SDKCONFIG_DEFAULTS=$D" "${@:-build}"
