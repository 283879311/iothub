# CMD-03 Phase A clean-cycle evidence (2026-11-25)

> 文档状态：CURRENT（限定时点证据）。源码基线 `407be43`；生产基线 `main` `956afcd`。

## 结论

阶段 A v2 证据链通过：clean-tree 测试镜像、设备日志、NVS 前后镜像和生产恢复身份链完整。

- 测试镜像均从干净 `407be43` 构建，`fw=1.3-407be43`。
- T13：设备侧 PASS。
- T19：设备侧 PASS。
- WAIT200：`pilot_pending`，不计为验收 PASS；200 ms 容差未冻结。
- NVS 前后 16 KiB 镜像逐字节一致。
- 生产 main 已恢复并核验 STA/IP/MQTT。

## 源码与构建

- Source: `feature/cmd03-devtest-helper` @ `407be4355a5a387d3b68e73bc1e5ebdcf5e4a793`
- T13 ELF `f00823efea0151748f57b217cbb6558ca030dc564af6340c859bf7fb10100371` / log `f00823efe`
- T19 ELF `d397a6d52088c1b3799025cdd9a68c9fc6669e5b21f4be33990be226dacea5df` / log `d397a6d52`
- WAIT200 ELF `2adcbe15b90d33c46d8b1d21f2f1591a3934cc8b409a56aca6e996f057cb08f8` / log `2adcbe15b`
- Production recovery: clean main 956afcd, ELF `c231c9501909eb9ce6dcdf687c812690eeba216502a576aa9ccc1c68eb1b2138`.

## Device results

- T13 PASS final: run `C23744B0000DA195`, op `op-913F5BC0C67B3277`; second finish PENDING; claim=1 restore=1 restore_ok=1; cancelled/OK; settle stable.
- T19 PASS final: run `923D7FDB000E16C0`, op `op-FCDA6C82F415B451`; job cleared/trial retained; required finish DONE; claim=1 restore=1; cancelled/OK.
- WAIT200 `pilot_pending`（不计为验收 PASS）：run `584C5972000DD72E`, op `op-31047E39FE8FBAC1`; 190090 us, 20/20 ticks, claim at return=RUNNING; first cleanup later OK; final cancelled/OK.

## NVS

- Before: `c1e04bc96a88f60f41c7b5d8a2a54473edca548d15d1d01dcbc8f7fc2f3f5dc6`
- After:  `c1e04bc96a88f60f41c7b5d8a2a54473edca548d15d1d01dcbc8f7fc2f3f5dc6`
- `cmp`: identical (16 KiB @ 0x9000).
- NVS binary images are **not published** because they may contain device configuration/credentials; only SHA-256 and comparison result are included.
- Flash operations were app-only at 0x10000; no NVS erase command.

## Host

- `tests/cmd_net_job`: T01–T23, T09a EXPECTED, 0 failed.
- `tests/operation_host`: S1–S5 50/50; A1/A2/A3/A4/A5/A5h/A7 PASS.

## 未覆盖

- Full device T01–T12/T14–T18/T20–T23: NOT_COVERED.
- WAIT200 tolerance: pilot_pending; 200 ms acceptance not frozen.
- `dev_test_helper.c` 950 lines: split deferred.

## Publication

- Serial/build/flash/device logs are sanitized; SSID, LAN IP, AP BSSID, device MAC, MQTT broker/client_id replaced.
- NVS binaries withheld; hashes only.
