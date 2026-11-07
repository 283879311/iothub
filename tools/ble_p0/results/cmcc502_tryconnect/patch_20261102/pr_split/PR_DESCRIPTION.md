# PR draft — wifi: fix tryconnect event ownership and teardown result races

## Base

- Branch: `test/c1-mainline`
- Base commit: `a35eef9`
- Functional patch: `functional_main_wifi.patch` (only `main/wifi.c`)

## Summary

Fix `network.tryconnect` event ownership so an old STA lifecycle cannot terminate a new candidate:

- add `IDLE → PRE_SCAN → CLEANUP_OLD → WAIT_OLD_DISCONNECT → CANDIDATE_STARTING → CANDIDATE_CONNECTING → DONE` phases;
- acquire tryconnect ownership before pre-scan; consume old disconnect events during cleanup;
- require candidate SSID/BSSID positive matching for `STA_CONNECTED`/`STA_DISCONNECTED`; BSSID conflict or missing identity is `UNKNOWN` and cannot terminate the candidate;
- `GOT_IP` validates current AP info; query failure/mismatch is ignored and waits for timeout or a valid event;
- enter `DONE` and freeze the result before teardown `esp_wifi_disconnect()`;
- restore old config, clear `s_wifi_sta_connecting`, resume auto-connect and schedule normal retry on every exit.

No MQTT, operation/defer, BLE protocol, profile schema or global auto-connect architecture changes.

## Hardware evidence

Hardware acceptance was run on the earlier formal build:

- BIN `abc193ce0d5768cbce6eaf20c381508e608de410652767fb285194f5ef8d1a73`
- ELF `86aeb79e28c56ffabd683c2b5cdbe3ab474e10a6e68498ef0f690b7a7785b6b4`
- A/B both flashed this build.

Covered:

- wrong-password, cancel, AP-not-found, timeout;
- candidate success → candidate GOT_IP → teardown → App apply;
- old STA connected switching candidate;
- deterministic old STA_CONNECTED/GOT_IP injection;
- same-SSID/different-BSSID conflict;
- query-failure → valid GOT_IP; query-failure → real failure; query-failure → timeout;
- dual-device BLE CMD/session + MQTT coexistence, isolated tryconnect, both recovered, 30s steady;
- no panic/WDT/brownout/abort; one boot `rst:` per device.

Details: `patch_20261102/PATCH.md`, `MERGE_REVIEW.md`.

## Clean-build equivalence note

The functional-only patch was independently built in `build-pr`:

- BIN `7aaf573029f42cbb4ca90ccd4f2021add562c978b11cdfe4bd9cd8d4ee2cd083`
- ELF `d10fefc3df3484284096292974a5e8694c78d4e5111df7c3ee2a0daea3cd0525`
- Size `0x118e90`; no C1 hook symbols.

**The clean build is not the same hash as the hardware-tested build.** Difference is removal of C1 test scaffolding (including the test-only GOT_IP AP-info parameter/flags); formal behavior is reviewed as equivalent, but a same-hash hardware rerun has not been performed.

CI should build the clean functional patch. Hardware acceptance remains attributed to `abc193…`; if strict same-hash acceptance is required, flash the clean build and rerun smoke + dual regression.

## Test-hook separation

`test_hooks.patch` contains only C1 test-scaffolding changes in `main/wifi.c`, `main/wifi.h`, `main/cmd.c` under `APP_C1_TEST_HOOKS`. It is not part of this functional PR. The formal build contains no injection symbols.

## Known boundaries (not merge blockers for this PR)

- Cancel during the 2s old-disconnect wait can still converge to `CONNECT_FAILED` instead of `CANCELLED` if the old event never arrives; recorded separately.
- `s_wifi_sta_connecting` remains a legacy independent flag; not used as event ownership and not refactored here.
- BLE DATA dual-device pressure and post-patch 6h soak are separate future stability work.

## Pre-PR checklist

- [x] Base commit remains `a35eef9`
- [x] Functional patch touches only `main/wifi.c`
- [x] Functional patch contains no C1/testhook code
- [x] Test-hook patch is separate and applies on top
- [x] Functional + test patch reconstructs the frozen tree byte-for-byte
- [x] Clean functional patch compiles independently
- [ ] Commit/PR created (not yet; explicitly pending)
- [ ] Same-hash clean-build hardware rerun (optional; not done)
