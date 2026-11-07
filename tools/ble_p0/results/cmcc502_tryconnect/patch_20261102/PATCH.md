# tryconnect ownership patch — 2026-11-02

## Status

- Implemented in `main/wifi.c` only.
- Build: PASS, `iothub.bin` `0x118e90` (41% partition free).
- Flashed to 65D8 (`/dev/ttyUSB0`, MAC `10:97:BD:D3:65:DA`) for regression.
- Flashed to C8E4 (`/dev/ttyUSB1`, MAC `b0:a7:32:28:c8:e4`) as well; full flash including otadata.
- **Not merged/committed.**
- A boot ELF SHA256 prefix `606c3f629...` matches `build/iothub.elf`; app version string remains `1.2-a35eef9` and compile-time field is stale, so use the ELF hash as the identity for this build.
- Scope kept to tryconnect state/event ownership/teardown; MQTT, operation/defer, BLE protocol, profiles, global auto-connect architecture unchanged.

## What changed

1. Added `app_wifi_try_phase_t`:
   `IDLE → PRE_SCAN → CLEANUP_OLD → WAIT_OLD_DISCONNECT → CANDIDATE_STARTING → CANDIDATE_CONNECTING → DONE`.
2. Added a spinlock-protected state API for phase/result/cancel/require_ip/candidate SSID+BSSID.
3. Moved tryconnect ownership before the pre-scan; pre-scan disconnect events no longer enter the normal auto-reconnect branch.
4. Added candidate SSID/BSSID matching:
   - BSSID match is strong.
   - Same SSID with BSSID conflict is `UNKNOWN`, not candidate failure.
   - Missing identity is `UNKNOWN`; it cannot terminate the candidate.
5. `STA_CONNECTED`/`STA_DISCONNECTED`/`GOT_IP` now consult the helper; `GOT_IP` additionally checks `esp_wifi_sta_get_ap_info()` and ignores query failure/mismatch.
6. Teardown now enters `DONE` before `esp_wifi_disconnect()`, so teardown disconnects cannot overwrite `OK/CANCELLED/TIMEOUT`.
7. All exits restore old config, clear `s_wifi_sta_connecting`, resume auto-connect and schedule the normal retry.

## Board regression (65D8)

Test script uses CMCC-502 with a deliberately wrong password, while old TQ2018 is absent/auto-reconnecting.

| Test | Result |
| --- | --- |
| Attempt 1: old STA auto-reconnecting + CMCC-502 | accepted → failed `wrong_password` in 15.96s |
| Attempt 2: immediately after attempt 1 | accepted → failed `wrong_password` in 16.15s |
| Serial evidence | `tryconnect: old disconnect consumed reason=201(NO_AP_FOUND)` during pre-scan; candidate failure logged as `reason=15(4WAY_HANDSHAKE_TIMEOUT)` |
| Old `reason=36` false failure | Not observed |
| Cancel while active | `cancelled` in 1.20s; teardown did not overwrite |
| Candidate not in scan | `failed / ap_not_found` in 6.50s |
| **Success path**: correct CMCC-502 password | **PASS**: associated at 644423ms; candidate IP `192.168.1.45` at 647643ms; tryconnect teardown `ASSOC_LEAVE` at 647653ms; App `network.apply` at 648663ms; normal reconnect got IP at 653573ms |
| Old STA **connected**, switch candidate | **PASS**: CMCC connected/IP first; two attempts logged `old disconnect consumed reason=8(ASSOC_LEAVE)`; both candidates returned real `wrong_password` |
| Injected old `STA_CONNECTED` | **PASS**: test build accepted injection but candidate remained `wrong_password` |
| Injected old `GOT_IP` | **PASS**: test build accepted injection but candidate remained `wrong_password` |
| Same SSID, different BSSID `GOT_IP` | **PASS**: treated `UNKNOWN`, candidate remained `wrong_password` |
| Candidate SSID+BSSID `GOT_IP` control | **PASS (hook control)**: injected event accepted, proving the injection path can reach the candidate branch |
| `GOT_IP` AP-info query failure, then valid candidate `GOT_IP` | **PASS**: after query failure operation stayed `connecting` (3.20s); later valid candidate event produced `got_ip` |
| `GOT_IP` query failure, then real candidate failure | **PASS**: stayed `connecting` after query failure; real candidate failure still returned `wrong_password` |
| `GOT_IP` query failure, no valid `GOT_IP` | **PASS**: candidate failure suppressed in test build; operation ended `timeout` at 11.30s, no false OK/password error |

Interpretation:

- 65D8 now really starts the CMCC-502 candidate and reports the real authentication failure.
- The old lifecycle event (`201`) is consumed during pre-scan, not misattributed.
- The teardown result is preserved for `wrong_password` and `cancelled`.
- **Success/GOT_IP/OK is closed**: the candidate received IP before teardown; the App then applied the trial, proving the tryconnect result was `OK` and the teardown did not overwrite it.
- Old connected/old success/BSSID-conflict events are deterministically ignored; the candidate positive control shows the hook exercises the intended branch.

## Dual-device regression (formal build, CMD/MQTT)

Both A and B run the same final formal ELF (`86aeb79e2...`, `iothub.bin` `abc193...`), both on CMCC-502.

| Test | Result |
| --- | --- |
| Initial A/B | both `sta_connected=true`, IP `.28`/`.45`, `mqtt_connected=true` |
| A tryconnect while B normal | A accepted → real `wrong_password` 11.24s; B remained WiFi+MQTT online for 3/3 samples |
| A recovery | A back online; MQTT had a normal backoff gap, then recovered |
| B tryconnect while A normal | B accepted → real `wrong_password` 10.60s; A remained WiFi+MQTT online for 3/3 samples |
| B recovery | MQTT had a normal backoff gap, then recovered |
| 30s steady state | A/B both `sta_connected=true`, `mqtt_connected=true`, IP stable for 3/3 samples |
| Resets/panic/WDT/brownout | none during run; each UART has one boot `rst:` only |
| MQTT disconnects | one expected disconnect per device during its own tryconnect; both recovered |
| BLE CMD/会话 | both links stayed responsive throughout; no BLE disconnect observed |

Scope note: this covers BLE CMD/session + MQTT coexistence, not a separate DATA-channel throughput test, and not a 6h soak.

## Not yet covered

- BLE DATA channel dual-device stress after this patch.
- 6h dual-device soak after this patch.

No T4-A or formal 6h acceptance claim is made from this patch.

## Evidence

Directory: `tools/ble_p0/results/cmcc502_tryconnect/patch_20261102/`

| File | Description | SHA-256 |
| --- | --- | --- |
| `build.log` | final build | `d99f3d09e317be4c3948124e8576280e3c8bf8c7371c0e618904d7f90c8f1034` |
| `wrong_password_old_event.log` | serial, two wrong-password attempts | `42854adfbeede893bc33a417d46287bab45bd3ec684ebc66e672935ed154e35d` |
| `wrong_password_old_event.out` | app-level operation results | `b03eddbc5098d5eca436dcae9268056d359870cebcdd1474c23550d4088bf9a1` |
| `cancel_ap_notfound.log` | serial, cancel + AP-not-found | `0080f0a1575d5b8cb7f80214555f056ad52a32768f31c12a7402276419bdd84b` |
| `cancel_ap_notfound.out` | app-level operation results | `defa5063bf13334d7403d44f69f59aa64a308a7d3b626b2566e5c82d2d66e84b` |
| `success_path.log` | serial, correct password success + apply + IP (full capture) | `9f73ff5c45d23c70f530ae37dd3b6388e74309a8fcd1a1c8db7889c900cc4b29` |

Event-boundary evidence: `tools/ble_p0/results/cmcc502_tryconnect/event_hooks_20261102/`

| File | Description | SHA-256 |
| --- | --- | --- |
| `old_connected.log` / `.out` | old STA connected, two candidate attempts | `f07ac27cd41d2d49e65349ba6d21c7b87ee0010e778022c08950b815e6718e17` / `402b758ad4d121a2b310e9ef167b02ca0ac1fb56fad395d52919ec046352d360` |
| `injected_events.log` / `.out` | old connected, old GOT_IP, BSSID conflict, positive control | `43bee4bc67f08bd36c7ed60672d396e4146e3917a77381cabf80bad01c51b268` / `d56c90e852530163d0e73bbc45536df0d55aef1c182701255f70eafbfe77e139` |
| `hook_build.log` | `APP_C1_TEST_HOOKS=ON` build | `39d02a81b83d4a0c1f8cfe6d6beb31f62ca8b132a353a675adce09ca89d3851e` |
| `formal_build_final.log` | final formal build (`APP_C1_TEST_HOOKS=OFF`) | `d99f3d09e317be4c3948124e8576280e3c8bf8c7371c0e618904d7f90c8f1034` |

GOT_IP query-failure evidence: `tools/ble_p0/results/cmcc502_tryconnect/got_ip_failure_20261102/`

| File | Description | SHA-256 |
| --- | --- | --- |
| `query_fail_then_valid.log` / `.out` | query failure then candidate GOT_IP | `58d46bd35010152523218cd0353b31f914352c46179339a3e36f99074fc6ce19` / `b4848ec6ac9a61c9d08fa92eb2be139019536e974aad201656822797b2295e94` |
| `query_fail_real_failure.log` / `.out` | query failure then real wrong-password failure | `9eb1958c66c60285247bf2102b27d4ea1cdb94e25d9cbf03a7c447107e261ff2` / `7a650a8bb854ca4398a3e4f16be96a3780cf9e1a7e3ae5c6f21670ce85f969b8` |
| `query_fail_timeout.log` / `.out` | query failure, no valid GOT_IP → timeout | `7ac316335c2704b11ca405562d37970d226303482026b88a22489d0cd77e3594` / `40d4861633c1db7fd706a415d122fc8a783093814d9d06c751de6a0cfcb53530` |

Dual-device evidence (final build): `tools/ble_p0/results/cmcc502_tryconnect/dual_regression_final_20261102/`

| File | Description | SHA-256 |
| --- | --- | --- |
| `A_uart0.log` / `B_uart0.log` | dual regression UART0 | `3c1eeb07d07c52a8a0c52d974080324893047a2f73448d56b65fb45c89658d5e` / `cf6abe8577d8b250f862dccc0b798a16e0e4e3d20391b7e42fae5d48feb99e95` |
| `dual_test.out` | dual tryconnect/recovery results | `6e07d1d09fd4aaf5685d26dd8c6a5c1b7ccf08443b04772d50a4b66c1ba23881` |
| `steady_30s.out` | 30s steady online samples | `960482b87e2784072f74aa1e393199743e31dbe9b6de1a9fca9bdeb14d8d7383` |

Binary: formal `build/iothub.bin`, SHA-256 `abc193ce0d5768cbce6eaf20c381508e608de410652767fb285194f5ef8d1a73`; ELF `86aeb79e28c56ffabd683c2b5cdbe3ab474e10a6e68498ef0f690b7a7785b6b4`. Both A and B were flashed with this formal build. The formal ELF contains no `app_wifi_test_inject_event` symbols; test hooks are compiled only in `build-hooks`.
