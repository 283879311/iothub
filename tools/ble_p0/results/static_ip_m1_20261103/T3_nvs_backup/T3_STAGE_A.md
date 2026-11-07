# T3 阶段 A：NVS 只读取证与双份备份（C8E4）

> 只读：read_mac / read_flash / 串口监听 / BLE config.get。未写 NVS、未刷固件、未改配置。

## 1. 设备与固件

| 项 | 值 |
| --- | --- |
| 设备 | C8E4，基础 MAC `b0:a7:32:28:c8:e4`（BLE 广播 `B0:A7:32:28:C8:E6`） |
| 固件 | M1 正式构建，`App version: 1.2-a35eef9-dirty` |
| 当前状态 | 静态 `192.168.1.50`，GOT_IP 11ms，MQTT connected |

## 2. 分区表（0x8000 读出）

```text
nvs      data nvs   0x9000  16K
otadata  data ota   0xd000   8K
phy_init data phy   0xf000   4K
ota_0    app  ota_0 0x10000 1920K
ota_1    app  ota_1 0x1f0000 1920K
storage  data littlefs 0x3d0000 64K
coredump data coredump 0x3e0000 32K
```

NVS：起始 `0x9000`，大小 `0x4000`（16 KiB），与分区表一致。

## 3. 双份备份与一致性

| 文件 | SHA-256 |
| --- | --- |
| `nvs_backup_1.bin` | `62cc574719585439635b1d6d1229d5c34e330c5c085cded78955e8fd02682389` |
| `nvs_backup_2.bin` | `62cc574719585439635b1d6d1229d5c34e330c5c085cded78955e8fd02682389` |

`cmp` 结果：两份字节完全一致。备份为 NVS 分区原始 16 KiB，不是配置导出文本。

当前配置快照（`current_config.json`）：`sta_use_static=true`、`192.168.1.50 / 255.255.255.0 / 192.168.1.1 / DNS 223.5.5.5 + 8.8.8.8`。

## 4. 字段定位（原始备份解析 + IDF 源码核对）

- 命名空间：`iothub`（ns index 1）；配置键：`runtime_cfg`（BLOB，页 0 entry 8，span=21）。
- BLOB 头（entry 8 data）：`dataSize=640`、`reserved=0xFFFF`、`dataCrc32=0x0619C999`。
- 640 字节 payload 从 `0x160` 开始，**整段连续**（entry 9 起每个 32 字节槽全部是 payload）。
- 字段偏移（payload 内）：

| 字段 | payload 相对偏移（绝对=+0x160） | 当前值 |
| --- | --- | --- |
| `sta_use_static` | `0x21F` | `0x01` |
| `sta_static_ip[16]` | `0x220` | `192.168.1.50` |
| `sta_static_gateway[16]` | `0x230` | `192.168.1.1` |
| `sta_static_netmask[16]` | `0x240` | `255.255.255.0` |
| `sta_static_dns1[16]` | `0x250` | `223.5.5.5` |
| `sta_static_dns2[16]` | `0x260` | `8.8.8.8` |

- 旧配置副本：页 1（seq=115）仍含 `192.168.43.213` 旧值；当前有效页为页 0（seq=118）。
- 校验算法已用备份反推验证（见 `CRC_NOTES.md`）：`crc32` 使用 `zlib.crc32(buf, 0xFFFFFFFF)`（无最终异或）。
  - blob `dataCrc32` = CRC(payload 640B) → 与 `0x0619C999` 一致；
  - entry 8 `crc32` = CRC(item[0:4] + key[16] + data[8]) → 与 `0xEDE22313` 一致。

## 5. 阶段 B 五个问题的初步答案

1. **分区信息**：`0x9000/0x4000`，分区表来源同上。
2. **sta_static_ip 存储格式**：位于 `runtime_cfg` blob 内的定长 `char[16]`（payload 偏移 `0x220`），不是独立 NVS 键；"删除字段"实际是**清零 16 字节**。
3. **sta_use_static 保持 true**：可以；它位于 `0x21F`，保持 `0x01`。启动时不校验字段完整性，进入 apply 后 `static_parse` 失败。
4. **校验字段**：需要重算两处 CRC——blob `dataCrc32`（payload 640B）与 entry 8 的 NVS entry CRC；BLOB_IDX `size=640/chunk=1` 不变；页头 CRC 不受影响；`app_config_t` 只有 `magic/version`，无应用级 CRC。
5. **救援是否改写 NVS**：不写。`main/wifi.c` 内唯一 `app_config_save` 是开机 `net_mode` 迁移（`net_mode` 已为 STA，不触发）；M1 的 apply/恢复链无 NVS 写入。

## 6. 阶段 B 拟定操作（未执行，待批准）

1. 由 `nvs_backup_1.bin` 生成 `nvs_bad_static.bin`：仅将 payload `0x220..0x22F` 清零，`0x21F` 保持 `0x01`；
2. 重算并写回 blob `dataCrc32`（`0x15C`）与 entry 8 `crc32`（`0x144`）；
3. 生成差异清单（预期仅这 18 字节区域变化），复核后写回 `0x9000`；
4. 读回 16 KiB 与生成镜像比对；重启执行 T3；
5. 测试后写回 `nvs_backup_1.bin` 并读回比对，确认恢复静态 `.50`。

风险：NVS 编辑器为自研；任何 CRC/偏移不符即停止，不重启测试，直接恢复备份。
