# S6 阶段 A · 分区表与烧录地址证据（2026-11-15）

本目录为 **git 跟踪**的证据快照（`artifacts/` 不入库，故把关键证据放这里）。

## 1. 文件

| 文件 | 说明 | SHA-256 |
| --- | --- | --- |
| `partition-table.bin` | 构建实际使用的分区表二进制（`build-release/partition_table/`） | `1c70e3068c81a8822da51fa4379a875548844027a9da6cfb1f471570012ad811` |
| `partition_decode.txt` | `gen_esp32part.py` 解码输出（ESP-IDF v6.0.3） | 见 `SHA256SUMS` |

## 2. 解码结果 vs `partitions.csv` 逐项对照

| 分区 | CSV | 二进制解码 | 一致 |
| --- | --- | --- | --- |
| nvs | data/nvs @ 0x9000, 0x4000 | 0x9000, 16K | ✓ |
| otadata | data/ota @ 0xd000, 0x2000 | 0xd000, 8K | ✓ |
| phy_init | data/phy @ 0xf000, 0x1000 | 0xf000, 4K | ✓ |
| **ota_0** | **app/ota_0 @ 0x10000, 0x1E0000** | **0x10000, 1920K** | ✓ |
| ota_1 | app/ota_1 @ 0x1F0000, 0x1E0000 | 0x1f0000, 1920K | ✓ |
| storage | data/littlefs @ 0x3D0000, 0x10000 | 0x3d0000, 64K | ✓ |
| coredump | data/coredump @ 0x3E0000, 0x8000 | 0x3e0000, 32K | ✓ |

**结论**：app 分区起始 `0x10000`、容量 `0x1E0000`（1920 KiB），由构建实际分区表二进制证实，
非 ESP-IDF 默认值推断。

## 3. 写入范围核算

- 冻结 BIN 大小 **1,175,632 B** = `0x11F050`（1148.1 KiB）< `0x1E0000`（1920 KiB） ✓
- 写入区间 = `[0x10000, 0x10000 + 0x11F050)` = **`[0x10000, 0x12F050)`**
- `ota_0` 区间 = `[0x10000, 0x10000 + 0x1E0000)` = `[0x10000, 0x1F0000)`
- **`0x12F050 < 0x1F0000` ⇒ 写入完全落在 `ota_0` 内**（余量 `0x1F0000-0x12F050 = 0xC0FB0`）
- 不触及 `otadata`(0xd000)、`nvs`(0x9000)、`ota_1`(0x1F0000) 之前空间，也不触碰 `storage`/`coredump`
- 不执行 `erase-flash`

## 4. 其他身份信息（正文见申请文档）

- 冻结 BIN：`iothub_a7e04b6a9.bin`，SHA-256 `047e4726455db505a5433bd3cb90a94022144add78361d6d8a52548e79f0ea41`
- 配套 ELF：`iothub_a7e04b6a9.elf`，SHA-256 `a7e04b6a98db47f05a06669738b2bc73ddd5df16ed7815f580a16085516f1ad7`，内嵌 `build_sha=a7e04b6a9`
- 源码提交：`856c669`；`CONFIG_IDF_TARGET=esp32`；flash 4MB
- esptool v5.6.0
