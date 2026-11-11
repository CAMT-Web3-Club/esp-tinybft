#!/usr/bin/env bash
# Usage: ./build_variant.sh <espnow|espnow_bcast|espnow_ps|udp_psnone|udp_psmin|udp_psmax> <replica|client> [build|flash|...]
# Builds into build_<variant>_<role>/ with its own sdkconfig, from overlays only.
set -e
V=${1:?variant}; R=${2:?role}; shift 2
B="build_${V}_${R}"
D="sdkconfig.defaults;sdkconfig.variant.$V;sdkconfig.role.$R;sdkconfig.measure"
# Optional extra overlays via EXTRA="sdkconfig.pm;sdkconfig.static_ip"
[ -n "$EXTRA" ] && D="$D;$EXTRA" && B="build_${V}_${R}_$(echo "$EXTRA" | tr ";." "__")"
[ -f sdkconfig.wifi.local ] && D="$D;sdkconfig.wifi.local"
exec idf.py -B "$B" -D "SDKCONFIG=$B/sdkconfig" -D "SDKCONFIG_DEFAULTS=$D" "${@:-build}"
