# H1-P1 / CMD-03 device smoke report

Date: 2026-11-23
Firmware under test: `b685bd5154eb265e4886a4de8ecf8f5f64422288`
Firmware reported by device: `1.3-b685bd5`, build_sha `ca103533d`, variant `mqtt_net_txn=on`

## Device identity and flash

- USB adapter: CH340 `1a86:7523` on `/dev/ttyUSB0` (no unique adapter serial)
- Chip: ESP32-D0WD-V3 rev v3.0
- MAC: `10:97:bd:d3:65:d8`
- device_id: `IotHub-65D8`
- BLE address: `10:97:BD:D3:65:DA` (Wi-Fi MAC + 2)
- Image: `iothub.bin`, SHA-256 `c5447f91962d4b65604789d4e524146fe48d947395163cecc5efb7c180488abe`
- Flash: `idf.py -p /dev/ttyUSB0 -B /tmp/h1-idf-build flash`, rc=0; bootloader/partition/ota_data/app written and verified; NVS not erased.
- Boot: SSID `CMCC-502`, IP `192.168.1.45`, MQTT `mqtt://139.9.93.245:21883`, client_id `iothub-1097BDD365D8`.

## Test client and scope

BLE P0 harness `tools/ble_p0/p0_acceptance.py` with default test admin credentials (not written to evidence), `--ssid CMCC-502 --no-final-config-set`.
No correct Wi-Fi password was supplied, so tryconnect/apply is expected to fail and no config apply is expected.
Serial capture was single-reader via `tools/capture_serial.py`.

## P0 results

| Case | Result | Notes |
|---|---|---|
| 1/2 scan + name | PASS | NUS UUID + `IotHubBLE-65D8`; phone UI not tested |
| 3 long scan x10 | PASS | long responses complete; `busy` responses handled by harness |
| 3b long config x10 | PASS | long config responses complete |
| 4 operation.get during fragmentation | PASS | duplicate same key returns same op; different key busy; async op non-blocking |
| 5a wrong password | PASS on focused retest | original P0 run interrupted by BLE disconnect; see below |
| 5b cancel race | PASS | 2/2 rounds: cancel + duplicate cancel = cancelled; terminal cancelled |
| 5c not found | PASS | operation.get/apply/cancel all `not_found` |
| 6 final config set | SKIP | `--no-final-config-set`; no config write |
| P0 core | PASS | `p0_core_pass=True`; `phone_ui_verified=False` |

### 5a wrong-password detail

The 5a operation started at device uptime ~121.7s and was accepted. The BLE central disconnected at uptime 134.781s (`bt_gatts: central disconnected (reason=0x222)`); firmware then:

```text
W cmd: rescue=restore_persistent op=op-DD709DE2C3BAFC46 (cleanup without commit)
I cmd: CLEANUP_DONE_OK op=op-DD709DE2C3BAFC46
I cmd: async network operation cancelled on BLE disconnect
W cmd: async tryconnect op=op-DD709DE2C3BAFC46 stale after wait (gen=1 state=7), no job touch
I wifi: cleanup complete op=op-DD709DE2C3BAFC46 attempts=1
```

The harness's reconnect then observed terminal `cancelled`, not `failed/wrong_password`, so the original P0 5a run is marked FAIL by the harness. This is a test-environment/link-loss limitation, not evidence of a wrong-password defect: in test 4 the earlier wrong-password operation reached `state=failed, error=wrong_password` (device uptime ~112.9s) while BLE remained connected.

### 5a focused retest (fresh BLE link)

A fresh BLE connection was established after a clean reboot, then only `network.tryconnect` with a deliberately wrong Wi-Fi password was executed and polled. No long preceding tests were run.

- operation_id: `op-0F22092FE1F7DB65`
- serial: `tryconnect failed: reason=15(4WAY_HANDSHAKE_TIMEOUT)` at uptime 56.751s
- `operation.get` terminal: `state=failed`, `error=wrong_password`, `finished_uptime_ms=56143`
- measured poll elapsed: 16.468s
- post-failure `network.cancel`: `operation_state` (correct; terminal op cannot be cancelled)
- BLE link remained connected through the terminal response; the central closed it only after the test completed (`reason=0x213`).
- Device then recovered to `CMCC-502` and MQTT `139.9.93.245:21883`.

This closes the 5a observation gap: the wrong-Wi-Fi-password terminal is `failed/wrong_password`; the original P0 5a remains an environment-interrupted run, not a product failure.

## CMD-03 evidence mapping

- CMD-03a/03b deterministic races: Host T01–T12 at PR head `b685bd5`: T01–T08, T10–T12 PASS; T09a EXPECTED; T09b PASS; 0 failed.
- Device cancel/cleanup ownership: 5b cancel race PASS 2/2; BLE-disconnect cleanup above shows `CLEANUP_DONE_OK`, restore-persistent, and stale worker rejected with `no job touch`.
- 200 ms cleanup claim wait: Host T09b timing PASS; device-side 200 ms measurement NOT_COVERED (no forced concurrent claim window in this smoke run).
- Full device concurrency acceptance (all T01–T12 race windows) NOT_COVERED; those remain Host-only deterministic evidence.

## Post-test state

- Device remained online: MQTT `139.9.93.245:21883` connected; SSID `CMCC-502`; DHCP.
- No successful apply; `final_config_set` skipped; persistent config unchanged by this run.
- The focused 5a retest also left SSID `CMCC-502` and MQTT `139.9.93.245:21883` unchanged; it did not write config.
- Device left running the flashed `b685bd5` image. No ESP-IDF rebuild was performed during the device run.

## Evidence hashes

See `h1-device-hashes.sha256` in this directory.
