# Device acceptance — firmware main `0e4d49c` (2026-11-24)

## Baseline

- Repo/worktree: `/tmp/iothub-main-merge`
- Branch: `main`
- HEAD: `0e4d49c2fe73165d4f01ebe2a4308893f3db45a2` (`Merge review/cmd03b-2026-11-22 into main`)
- Build: BIN `f6ff92b5ad7547bb30cbbaf5a77d0b4026983449a51eacb7250e22f7c824921f`
  (ELF `659cf7f1c53cc567bfdd3bc90495020d6dc0fc1cbaad624210504bb115cbbbfb`)
- Image flashed to the board on `/dev/ttyUSB0`; NVS was not erased.
- Device identity (BLE `device.info`): `IotHub-65D8`, firmware `1.3-0e4d49c`,
  `build_sha=659cf7f1c`, build variant `mqtt_net_txn=on`.
  BLE advertised address observed: `<ble-mac>`.

## Test entry

- Script: `main_accept.py` (BLE; cancel/duplicate-cancel, BLE-disconnect cleanup,
  recovery status). The script is retained only in the local test workspace and
  is not included in this publication bundle.
- Serial: single-reader capture `capture_serial.py /dev/ttyUSB0`
- No persistent config field was changed. `network.tryconnect` used a deliberately
  wrong password and was always cancelled; cleanup restored the persistent profile.
- No password or token values are published. The BLE username is masked as
  `<ble-user>` in the published client transcript.

## Results

### PASS — cancel at `connecting` + duplicate cancel (2 rounds)

| Round | op | tryconnect | cancel | duplicate cancel | terminal |
|---|---|---|---|---|---|
| 1 | `op-817FABFBE2667274` | accepted | `cancelled` | `cancelled` | `cancelled`, `cleanup_state=ok`, `error=cancelled` |
| 2 | `op-8C26C09891553922` | accepted | `cancelled` | `cancelled` | `cancelled`, `cleanup_state=ok`, `error=cancelled` |

Serial for each round shows `rescue=restore_persistent`, exactly one
`CLEANUP_DONE_OK`, one `defer_end site=op_finish/terminal`, and
`finish ... -> cancelled/cancelled outcome=0 (DONE_OK)`. The worker later logged
`stale after wait ... no job touch`; the Wi-Fi cleanup completed separately.
No duplicate cleanup claim or failure was logged.

### PASS — BLE disconnect with active operation

- op `op-F8713D39989362CF`; BLE link dropped while tryconnect was connecting.
- Device automatically finished it: `cancelled`, `cleanup_state=ok`,
  `error=cancelled`.
- Serial: `rescue=restore_persistent`, `CLEANUP_DONE_OK`,
  `defer_end ...` once, `finish ... cancelled outcome=0`, then
  `async network operation cancelled on BLE disconnect`.
- Wi-Fi cleanup completed asynchronously afterwards; no stuck owner was observed.

### PASS — network/MQTT recovery after tests

Final `network.status`:
`sta_connected=true`, `sta_has_ip=true`, IP `<device-ip>`,
SSID `<wifi-ssid>`, `internet_verified=true`, `net_verify_stage=mqtt`,
`mqtt_connected=true`, `last_disconnect=none`.

### PASS (observation) — terminal/defer invariants on device

- 3 terminal finish paths produced exactly 3 `defer_end` events.
- `CLEANUP_DONE_FAIL=0`, `cleanup wait timeout=0`, `cleanup claim anomaly=0`.
- No panic/assert/stack overflow in the captured boot-and-test log.
- `cleanup_state=ok` was observed while asynchronous Wi-Fi cleanup was still
  running, consistent with the contract wording that `ok` means cleanup
  claim/dispatch, not Wi-Fi restore completion.
- Device-internal cleanup latency, measured from `CLEANUP_DONE_OK` (or the
  immediately following `finish`) to `wifi: cleanup complete`, was
  **2.47–4.23 seconds**:

  | Round | op | `CLEANUP_DONE_OK` → cleanup complete | `finish` → cleanup complete |
  |---|---|---:|---:|
  | 1 | `op-817FABFBE2667274` | 4.23 s | 4.21 s |
  | 2 | `op-8C26C09891553922` | 3.77 s | 3.76 s |
  | 3 | `op-F8713D39989362CF` | 2.48 s | 2.47 s |

  `cleanup_state=ok` only means the cleanup claim/dispatch succeeded; it does
  not mean the asynchronous Wi-Fi cleanup or persistent-config restore has
  completed. It must not be used as a signal that the next network operation
  can start immediately. Network truth remains `network.status` / `config.get`.

## Coverage against Host T01–T23

- PASS: external cancel/duplicate-cancel, BLE-disconnect cleanup, persistent
  restore request, MQTT defer end once, `cleanup_state` terminal visibility,
  recovery.
- PARTIAL: T13 first-writer/defer-once (sequential duplicate only; true
  simultaneous first-writer PENDING not triggered); T19 trial-only REQUIRED
  restore (restore request observed, but not the Host's trial-only/no-job setup).
- NOT_COVERED on device: T01–T12, T14–T18, T20–T23. These depend on internal
  hooks, two simultaneous cleanup claimants, fault injection, `COMMITTING`, or
  a stalled claimant. The BLE API serializes commands and cannot reproduce them;
  no 200 ms cleanup-wait timeout was entered.
- Device-side 200 ms bounded-wait measurement: **NOT_COVERED**. Host T09b/T20
  remain the evidence for that path. Do not report it as device PASS.

## Evidence

This review bundle is a **sanitized publication copy**. Wi-Fi SSID, LAN IP,
AP BSSID, BLE/USB MAC addresses, MQTT broker, and MQTT client_id were replaced;
protocol messages, operation IDs, states, timings, and results are unchanged.

- `serial.txt` — full boot + tests + recovery
- `info.txt` — device.info and network.status before tests
- `client.txt` — client-side BLE transcript/results
- `SHA256SUMS` — hashes of this bundle

Raw local originals (not published) have SHA-256:

```text
a1c320cc66780e74330b62930857e19f535fa22fb8eda6185098da222534e94a  DEVICE-REPORT.md
8a472c1f6f6ef484a45377ce0847edd90df1388f8e89fce7bc2d7994317beec7  main_accept.log
de3ba12f2a3329280f088e296971f2a7199f2c7428f8f2487f89b6db930a4049  main_info.log
852a4aa433f15d9152e9384f77ea800a25bafc4c3f4203da9b92ed8fcda76c07  main_serial.log
```
