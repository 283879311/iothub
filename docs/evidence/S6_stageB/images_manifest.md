# S6 阶段 B · 镜像 manifest（2026-11-15）

> 供烧录授权与回滚使用；**完整 SHA-256，不使用缩写**。

## 1. FI 镜像（阶段 B 烧录对象）

| 项 | 值 |
| --- | --- |
| 文件 | `artifacts/cmd04_flash_20261115/images/iothub_fi_efc7b5ba5.bin` |
| **BIN SHA-256** | `ca71c2426d24012474f31aee7d3835810a51512015d7801b3e43e7758a2e9dd3` |
| BIN 大小 | 1,179,216 B = `0x11FE50` |
| 配套 ELF | `iothub_fi_efc7b5ba5.elf` |
| **ELF SHA-256** | `efc7b5ba53330978bc69ae906d9eab7032bd1be3a057231b949be199fc3da4e6` |
| 内嵌 build_sha | `efc7b5ba5` |
| 编译开关 | `-DAPP_WIFI_FAULT_INJECT=ON` |
| 源码提交 | 见 §3（含 cleanup_hold 注入） |
| 写入区间 | `[0x10000, 0x12FE50)` ⊂ `ota_0 [0x10000, 0x1F0000)` ✓（余量 `0xC01B0`） |

## 2. 功能回滚镜像（阶段 B 结束回滚对象）

| 项 | 值 |
| --- | --- |
| 文件 | `artifacts/cmd04_flash_20261115/images/iothub_a7e04b6a9.bin` |
| **BIN SHA-256** | `047e4726455db505a5433bd3cb90a94022144add78361d6d8a52548e79f0ea41` |
| BIN 大小 | 1175632 B |
| 配套 ELF | `iothub_a7e04b6a9.elf`（sha256 `a7e04b6a98db47f05a06669738b2bc73ddd5df16ed7815f580a16085516f1ad7`） |
| 内嵌 build_sha | `a7e04b6a9` |
| 写入区间 | `[0x10000, 0x12F050)` ⊂ `ota_0` ✓ |

## 3. 分区与构建身份（沿用阶段 A 证据）

- 分区表：`docs/evidence/S6_stageA/partition-table.bin`（sha256 `1c70e3068c81a8822da51fa4379a875548844027a9da6cfb1f471570012ad811`）
  → 解码证实 `ota_0, app, 0x10000, 1920K`
- `CONFIG_IDF_TARGET=esp32`；flash 4MB；esptool v5.6.0
- 源码：`main/wifi.c` 的 S6 修复 + FI 注入（`restart_wake_fail` / `cleanup_hold`）

## 4. 校验方式

- 门禁：`preflight_flash.py --bin <BIN> --expect-bin-sha256 <完整 SHA>`（须 rc=0）
- 烧录：`tools/flash_frozen.sh <BIN> <完整 SHA>`（端口/芯片/地址/上限锁定；私有副本前后校验）
- 烧录后：`device.info.build_sha` 反证（FI 应为 `efc7b5ba5`）
