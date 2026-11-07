# CMCC-502 tryconnect failure reproduction (2026-11-02)

## Result

65D8 (`10:97:BD:D3:65:DA`) received `network.tryconnect ssid='CMCC-502'`, but the candidate connection never reached association. The firmware reported:

```text
I (71153) wifi: tryconnect: ssid='CMCC-502' pwd_len=10 threshold=2(WPA) pmf(capable=1,required=0) scan=1
I (71153) wifi: tryconnect failed: reason=36(OTHER)
I (71163) wifi: tryconnect ssid='CMCC-502' result=3 require_ip=1
W (71163) wifi: STA disconnected: reason=36(OTHER) retries=3 has_sta=1 profile=0
```

- `result=3` = `APP_WIFI_TRY_CONNECT_FAILED` (`main/wifi.h`).
- `reason=36` = `WIFI_REASON_STA_LEAVING` (`esp_wifi_types_generic.h`), a local station-leaving reason, not an AP/password reason.
- There is no `wifi:new`, `wifi:state: init -> auth`, or `wifi:connected` for CMCC-502 between the candidate config log and the failure; the radio did not start a candidate association.
- The old config remains `TQ2018`/`TQ2017`; no `CMCC-502` profile was applied. This is expected after a failed `tryconnect`.

## Root cause (firmware race)

`app_wifi_try_connect_common()` in `main/wifi.c`:

1. snapshots only `s_wifi.sta_connected` into `old_connected` (line ~1727);
2. sets `s_wifi_sta_connecting = true` and calls `esp_wifi_disconnect()` (lines ~1736-1738);
3. waits for the old disconnect event **only if `old_connected` is true** (lines ~1739-1741);
4. then sets the candidate and `s_tryconnect_connecting = true` (lines ~1754-1756).

When the old STA is not `sta_connected` but an auto-connect/scan attempt is still in flight, `esp_wifi_disconnect()`/the old pending disconnect produces `reason=36 (STA_LEAVING)`. That event can be delivered after `s_tryconnect_connecting` is set, so the event handler's tryconnect branch attributes it to the candidate and sets `APP_WIFI_TRY_CONNECT_FAILED`. The log ordering (candidate log and failure at the same millisecond, no association) matches this race.

The app's later `last_disconnect=no_saved_ap` is from the restored old auto-connect path, not from the CMCC-502 candidate; it is not the candidate failure reason.

## Fix direction

- Snapshot both old connected and old connecting state before changing `s_wifi_sta_connecting`.
- Drain/wait for the intentional old disconnect event (including `reason=36`) before setting `s_tryconnect_connecting=true` and starting the candidate.
- Or add a tryconnect generation/flag so the event handler ignores the first local `STA_LEAVING` event belonging to the old link.
- Regression: STA down / auto-connect in progress + `tryconnect` must not return `result=3 reason=36` before any candidate association; then run CMCC-502 against the real AP.

## Evidence

Raw serial: `B_tryconnect_cmcc502_20261102.log` (65D8 UART0, 19:35–19:38).
