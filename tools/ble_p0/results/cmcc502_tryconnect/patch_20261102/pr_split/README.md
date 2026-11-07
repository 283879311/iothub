# PR split — 2026-11-02

## Files

### Functional patch

`functional_main_wifi.patch`

- Contains only `main/wifi.c`.
- Base: `HEAD a35eef9`.
- Applies cleanly to HEAD (`git apply --check` passed).
- Does not contain `APP_C1_TEST_HOOKS`, `s_wifi_test_*`, or test-injection code.
- SHA-256: `27125eca85dcf85a24d412829f23b2862c17f22a9e4cdf0093267b9e47d3fa49`

### Test-hook patch

`test_hooks.patch`

- Contains `main/wifi.c` C1-only hooks plus `main/wifi.h` / `main/cmd.c` C1-only command.
- Applies on top of the functional patch (`git apply --check` passed).
- Applied functional + test patch reconstructs the current frozen working tree byte-for-byte (`wifi.c`, `wifi.h`, `cmd.c` all matched).
- SHA-256: `d6d6f420fc067ed5a690de4299a576da61232947bbda4b6d766e0787da13ac56`

## Build verification

Functional-only build (C1 hooks OFF, separate `build-pr`):

- `iothub.bin`: `7aaf573029f42cbb4ca90ccd4f2021add562c978b11cdfe4bd9cd8d4ee2cd083`
- `iothub.elf`: `d10fefc3df3484284096292974a5e8694c78d4e5111df7c3ee2a0daea3cd0525`
- Size: `0x118e90` (41% free)
- No `app_wifi_test_inject_event` / `app_wifi_test_parse_bssid` symbols.

Build log: `functional_only_build.log` (`6a7d2afb063e703c0bdc239604add4d3dd1de1f10749f0c7b9cc6366300458e2`).

## Evidence-to-binary note

The board acceptance evidence (success, P1, event injection, dual regression) was collected on the earlier formal build:

- BIN `abc193ce0d5768cbce6eaf20c381508e608de410652767fb285194f5ef8d1a73`
- ELF `86aeb79e28c56ffabd683c2b5cdbe3ab474e10a6e68498ef0f690b7a7785b6b4`

The clean functional patch removes only the C1 test scaffolding from the formal source (including the unused GOT_IP test-info parameter/flags); formal behavior is reviewed as equivalent, but the clean `build-pr` binary hash necessarily differs from `abc193…`.

Before PR merge, either:

1. Treat `abc193…` as the hardware-test build and have CI build the clean patch for the PR, or
2. Flash the clean `build-pr` binary to A/B and re-run at least smoke + dual regression for a strict hash-to-evidence mapping.

Current A/B still run `abc193…`; no clean binary has been flashed.

## Status

- Patches are archived only; **not committed, not merged, no PR opened**.
- Test-hook build remains independently reproducible from `main/wifi.c` + `wifi.h` + `cmd.c` under `APP_C1_TEST_HOOKS`.
- Functional PR has no dependency on C1 hooks.
