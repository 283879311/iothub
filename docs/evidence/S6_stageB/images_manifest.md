# S6 阶段 B · 镜像 manifest（2026-11-15）

> 供烧录授权与回滚使用；**完整 SHA-256，不使用缩写**。
> 二进制实体已入库到本目录 `bin/`，可独立计算哈希复核。

## 1. FI 镜像（阶段 B 烧录对象）

| 项 | 值 |
| --- | --- |
| 仓库内实体 | `docs/evidence/S6_stageB/bin/iothub_fi_55c00628f.bin` |
| 工作副本 | `artifacts/cmd04_flash_20261115/images/iothub_fi_55c00628f.bin` |
| **BIN SHA-256** | `1a7ff649e79319c091880351d2a017270581fb74090c5c04ed57dba995fb0efc` |
| BIN 大小 | 1,179,424 B = `0x11FF20` |
| 写入区间 | `[0x10000, 0x12FF20)` ⊂ `ota_0 [0x10000, 0x1F0000)` ✓（余量 `0xC00E0`） |
| 配套 ELF | `artifacts/cmd04_flash_20261115/images/iothub_fi_55c00628f.elf` |
| **ELF SHA-256** | `55c00628f9bba2edc2246a4967dc29f6df1de9770654a357c16081c6cfdfe41b` |
| 内嵌 build_sha | `55c00628f` |
| 编译开关 | `-DAPP_WIFI_FAULT_INJECT=ON` |
| **源码提交** | **`bc6d362`**（`cleanup_hold` 可中断版 + harness 修正） |

## 2. 功能回滚镜像（阶段 B 结束回滚对象）

| 项 | 值 |
| --- | --- |
| 仓库内实体 | `docs/evidence/S6_stageB/bin/iothub_a7e04b6a9.bin` |
| 工作副本 | `artifacts/cmd04_flash_20261115/images/iothub_a7e04b6a9.bin` |
| **BIN SHA-256** | `047e4726455db505a5433bd3cb90a94022144add78361d6d8a52548e79f0ea41` |
| BIN 大小 | 1,175,632 B = `0x11F050` |
| 写入区间 | `[0x10000, 0x12F050)` ⊂ `ota_0` ✓ |
| 内嵌 build_sha | `a7e04b6a9`（阶段 A 已实测运行并通过 R1/R2/R3/R5） |

## 3. 分区与构建身份

- 分区表：`docs/evidence/S6_stageA/partition-table.bin`
  （SHA-256 `1c70e3068c81a8822da51fa4379a875548844027a9da6cfb1f471570012ad811`）
  → 解码证实 `ota_0, app, 0x10000, 1920K`
- `CONFIG_IDF_TARGET=esp32`；flash 4MB；esptool v5.6.0
- 两镜像同源（`bc6d362` 的 `main/wifi.c`），仅 `APP_WIFI_FAULT_INJECT` 开关不同

## 4. 校验方式

```
preflight_flash.py --bin <BIN> --expect-bin-sha256 <完整 SHA>   # 须 rc=0
tools/flash_frozen.sh <BIN> <完整 SHA>                          # 锁定端口/芯片/地址/上限
# 烧录后：device.info.build_sha 反证（FI=55c00628f，回滚=a7e04b6a9）
```

`bin/SHA256SUMS` 为上述两 BIN 的哈希清单。
