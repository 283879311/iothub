# tryconnect patch — merge review（2026-11-02）

## Scope

- Functional fix: `main/wifi.c` (tryconnect ownership/phase/event matching/teardown).
- Test-only hooks: `main/wifi.h`, `main/cmd.c`, `main/wifi.c` under `APP_C1_TEST_HOOKS=ON`; absent from the formal ELF.
- No MQTT, operation/defer, BLE command protocol, profile data structure, or global auto-connect architecture changes.

Final formal build:

- BIN `build/iothub.bin`: `abc193ce0d5768cbce6eaf20c381508e608de410652767fb285194f5ef8d1a73`
- ELF `build/iothub.elf`: `86aeb79e28c56ffabd683c2b5cdbe3ab474e10a6e68498ef0f690b7a7785b6b4`
- A and B both flashed with this build; boot ELF prefix `86aeb79e2...`.

## 1. Lock protection

- `s_tryconnect_active`, `phase`, `result`, `require_ip`, `cancel_requested`, candidate SSID/BSSID are accessed through `app_wifi_tryconnect_state_lock()` (`s_tryconnect_mux`).
- Event-group pointer creation and reads are under the same lock; `xEventGroupSetBits()` is called outside the lock.
- No `esp_wifi_*`, `esp_netif_*`, logging or blocking wait is called while holding the state lock.
- `s_wifi_sta_connecting` remains a legacy, separately accessed flag. It is not used as event ownership; tryconnect `active` suppresses monitor scheduling. This is unchanged scope and not a new race introduced by the patch, but it is a known legacy risk if future code uses it as a synchronization primitive.

Review status: **no blocking lock issue in the patch.**

## 2. Event ownership

- Phase table: `IDLE → PRE_SCAN → CLEANUP_OLD → WAIT_OLD_DISCONNECT → CANDIDATE_STARTING → CANDIDATE_CONNECTING → DONE`.
- Pre-scan owns the operation; old disconnect events cannot enter normal auto-reconnect.
- Candidate `STA_CONNECTED` / `STA_DISCONNECTED` events must positively match candidate SSID or BSSID.
- BSSID conflict with the same SSID becomes `UNKNOWN`; it cannot terminate the candidate.
- Missing identity becomes `UNKNOWN`; it cannot terminate the candidate.
- No unconditional ignore of `STA_LEAVING` (reason 36): in `CANDIDATE_CONNECTING` it is classified like any other candidate reason.
- `GOT_IP` uses `esp_wifi_sta_get_ap_info()` and ignores query failure/mismatch; it does not report success or a wrong-password failure on query failure.

Review status: **event ownership boundary accepted.**

## 3. Terminal result and teardown

- `finish()` sets the result and enters `DONE` under lock before `esp_wifi_disconnect()`.
- Event callbacks in `DONE` cannot modify the result.
- After teardown, `finish()` restores old configuration, clears `s_wifi_sta_connecting`, releases ownership, resumes auto-connect and schedules the normal retry.
- Board evidence covers `OK`, `WRONG_PASSWORD`, `CANCELLED`, `AP_NOT_FOUND`, `TIMEOUT`, and teardown after success.

Review status: **no blocking terminal/teardown issue.**

## 4. Automatic connection recovery

- Final formal-build dual regression: A/B both online, isolated tryconnect, temporary MQTT backoff, both recovered; 30s steady state 3/3.
- Expected per-device MQTT disconnect during its own tryconnect; no permanent disconnect.
- No panic/WDT/brownout/abort; each UART has only the boot `rst:`.

Review status: **recovery path accepted for the tested AP/configuration.**

## 5. Merge hygiene / residual risks

1. **Test-hook code is not part of the functional patch.** It is in `wifi.h`/`cmd.c`/`wifi.c` only under `APP_C1_TEST_HOOKS`; formal ELF has no hook symbols. For a production merge, either omit the hook files from the merge commit or keep them clearly separated as test-only changes.
2. `s_wifi_sta_connecting` remains outside the new state lock; this is pre-existing and should not be repurposed as event ownership.
3. Cancel during the 2s old-link wait can still converge to `CONNECT_FAILED` instead of `CANCELLED` if the old event never arrives; current tests cover cancel during pre-scan, not this narrow window.
4. `strlen(ap_info.ssid)` assumes a terminated AP SSID; existing code, not changed.
5. BLE DATA dual-device throughput and post-patch 6h soak are not covered.

## Recommendation

**Functional tryconnect acceptance: PASS.**
**Merge: hold for code review / PR; no commit yet.**
The functional diff should be reviewed independently from the C1 test hooks. No further change to `main/wifi.c` is proposed before that review.

## PR split status

- `pr_split/functional_main_wifi.patch`: only `main/wifi.c`; applies cleanly to HEAD.
- `pr_split/test_hooks.patch`: C1-only changes in `main/wifi.c`, `main/wifi.h`, `main/cmd.c`; applies on top of functional patch.
- Applying both reconstructs the frozen working tree exactly.
- Functional-only build PASS: BIN `7aaf5730…`, ELF `d10fefc3…`, size `0x118e90`.
- Hardware evidence remains on `abc193…`/`86aeb…`; the clean functional build differs only by test scaffolding but has a different hash. Before merge, either have CI build the clean patch, or flash the clean build and rerun smoke/dual for strict hash-to-evidence mapping.
- No commit, merge, or PR was created.
