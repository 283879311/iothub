# T3 阶段 B0：离线故障镜像生成与校验（未连接设备）

> 本阶段仅使用已备份的 16 KiB 原始 NVS 在主机上生成镜像；未连接 C8E4、未写 flash、未重启。

## 1. 产物与哈希

| 文件 | 内容 | SHA-256 |
| --- | --- | --- |
| `nvs_page0_bad.bin` | 4 KiB，仅 page0（拟写入 0x9000） | `50944dab89a55304b19a91d17c3a31e07dd4b9bbfe7ba19837c2224a0d6c1059` |
| `nvs_bad_static.bin` | 16 KiB 全镜像，page1–3 与原始完全一致 | `78e2af1d2cc19d176ee23da3cc3a2b4f8ae45da34fb05521a06f0703f44f9bae` |
| `nvs_backup_1.bin` | 原始 16 KiB | `62cc574719585439635b1d6d1229d5c34e330c5c085cded78955e8fd02682389` |

`nvs_bad_static.bin[4096:] == nvs_backup_1.bin[4096:]`：**True**（page1–3 未改）。

## 2. 字节差异（page0）

允许变化区域 24 字节；实际变化 **22 字节**，4 段：

```text
0x144..0x147  entry CRC     1323e2ed -> d973c8af
0x15c..0x15f  blob dataCrc  99c91906 -> 22d94cf4
0x380..0x38b  ip 字符串     3139322e3136382e312e3530 -> 000000000000000000000000
0x38d..0x38e  ip 残留       6f40 -> 0000
```

`0x38C`、`0x38F` 原为 `00` 不变；`sta_use_static`（0x37F）保持 `01`。

## 3. CRC

| 项 | 原始 | 修改后 |
| --- | --- | --- |
| blob `dataCrc32` @0x15C | `0x0619C999` | `0xF44CD922` |
| entry 8 `crc32` @0x144 | `0xEDE22313` | `0xAFC873D9` |

算法：`zlib.crc32(buf, 0xFFFFFFFF)`（等价 `esp_rom_crc32_le(0xFFFFFFFF, …)`，无最终异或）。
- entry CRC 范围：`item[0:4] + key[16] + data[8]`（排除自身 CRC 字段）；
- blob CRC 范围：后续 640 字节 payload。

## 4. 结构一致性（独立解析校验）

| 检查 | 结果 |
| --- | --- |
| page 状态 | `0xFFFFFFFE`（valid），未变 |
| page seq | `118`，未变 |
| 页头 CRC | stored `0xAEE60426` = calc `0xAEE60426` |
| entry bitmap | 与原始逐字节一致 |
| `runtime_cfg` | entry 8，span=21，dataSize=640，reserved=0xFFFF |
| blob CRC | stored = calc = `0xF44CD922` |
| entry 8 CRC | stored = calc = `0xAFC873D9` |
| 其他 item entry（3 个） | CRC 全部有效 |
| payload 语义 | magic `HTOI`、version=11、`sta_use_static=0x01`、ip 16 字节全零 |
| 未改字段 | gateway/mask/dns1/dns2 内容保持原值 |

BLOB payload = 640 字节 = 20 个 32 字节数据 entry + 1 个 metadata entry，span=21 自洽。

## 5. 设备写入方案（未执行）

- 只写 page0：`esptool write_flash 0x9000 nvs_page0_bad.bin`（4 KiB 扇区擦除后写入）；
- page1–3 不写；
- 写入后立即读回 4 KiB 与 `nvs_page0_bad.bin` 比对，不一致即用 `nvs_backup_1.bin` 的 page0 恢复并停止；
- 重启后若出现任何 page1 回退迹象（如仍连 192.168.43.x、或日志无 `static ip configured` 的失败路径），判 T3 失败，立即恢复，不把回退当救援成功。

## 6. 恢复方案

- 正常路径：写回原始 page0（`nvs_backup_1.bin[0:4096]`），读回比对；
- 若发现 page0 之外任何字节变化：写回完整 16 KiB `nvs_backup_1.bin`，读回比对；
- 恢复后重启确认 `sta_use_static=true`、静态 `192.168.1.50`、MQTT 在线。

## 7. 当前状态

镜像已生成并校验；**未写设备、未重启、未连接 C8E4**。等待阶段 B 写入批准。
