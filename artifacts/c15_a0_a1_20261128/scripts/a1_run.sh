#!/bin/bash
# A1 launcher: credentials supplied via environment; never stored here.
set -euo pipefail
export WIFI_SSID="${WIFI_SSID:?set WIFI_SSID}"
export WIFI_PASSWORD="${WIFI_PASSWORD:?set WIFI_PASSWORD}"
export IOTHUB_BLE_USER="${IOTHUB_BLE_USER:?set IOTHUB_BLE_USER}"
export IOTHUB_BLE_PASSWORD="${IOTHUB_BLE_PASSWORD:?set IOTHUB_BLE_PASSWORD}"
exec python3 "$(dirname "$0")/a1_begin_test.py"
