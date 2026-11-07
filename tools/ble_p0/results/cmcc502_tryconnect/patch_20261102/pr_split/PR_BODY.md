## Summary

This PR targets `main` at the user's explicit request.

Because `main` is behind the test-candidate baseline `a35eef9`, the compare includes the full `test/c1-mainline` candidate history since main (133 commits, 289 files). The only commit added by this branch after that baseline is:

- `ea90825` — `fix(wifi): bind tryconnect events to the active candidate lifecycle` (only `main/wifi.c`)

The C1 test-hook changes are **not** in this branch/commit.

## tryconnect fix

- Explicit phases: `IDLE → PRE_SCAN → CLEANUP_OLD → WAIT_OLD_DISCONNECT → CANDIDATE_STARTING → CANDIDATE_CONNECTING → DONE`.
- Ownership acquired before pre-scan; old STA disconnect events cannot enter normal auto-reconnect or terminate the new candidate.
- Candidate `STA_CONNECTED`/`STA_DISCONNECTED` require positive candidate SSID/BSSID matching; BSSID conflict or missing identity is `UNKNOWN`.
- `GOT_IP` validates current AP info; query failure/mismatch waits rather than reporting OK or a false failure.
- `DONE` freezes the result before teardown `esp_wifi_disconnect()`.
- Unified restore of old config, auto-connect resume and normal retry scheduling.

## Verification

Hardware acceptance was performed on formal build:

- BIN `abc193ce0d5768cbce6eaf20c381508e608de410652767fb285194f5ef8d1a73`
- ELF `86aeb79e28c56ffabd683c2b5cdbe3ab474e10a6e68498ef0f690b7a7785b6b4`
- A/B both flashed; same ELF hash.

Covered: success/GOT_IP/teardown, wrong password, cancel, AP-not-found, old STA connected, injected old STA_CONNECTED/GOT_IP, same-SSID/different-BSSID, query-failure (valid/failure/timeout), dual-device CMD/MQTT isolation and recovery, 30s steady.

Clean functional-only build (C1 hooks OFF) also compiled:

- BIN `7aaf573029f42cbb4ca90ccd4f2021add562c978b11cdfe4bd9cd8d4ee2cd083`
- ELF `d10fefc3df3484284096292974a5e8694c78d4e5111df7c3ee2a0daea3cd0525`

The clean build is not the same hash as the hardware-tested build; difference is removal of C1 test scaffolding. A same-hash hardware rerun has not been performed; CI should build the clean patch for this PR.

## Known boundaries

- Cancel during the 2s old-disconnect wait can still converge to `CONNECT_FAILED` rather than `CANCELLED` if the old event never arrives.
- `s_wifi_sta_connecting` remains a legacy independent flag, not event ownership.
- BLE DATA dual-device pressure and post-patch 6h soak remain separate stability work.

## Review materials

- `tools/ble_p0/results/cmcc502_tryconnect/patch_20261102/MERGE_REVIEW.md`
- `tools/ble_p0/results/cmcc502_tryconnect/patch_20261102/pr_split/PR_DESCRIPTION.md`
