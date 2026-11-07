# T4 执行卡：静态字段格式非法 → 启动 DHCP 救援（未执行）

> 仅准备执行卡与离线镜像；未写 NVS、未重启、未连接设备。
> 设备：C8E4（`/dev/ttyUSB1`，MAC `b0:a7:32:28:c8:e4`）；65D8 不参与。

## 1. 非法字段与字节表示

- 字段：`sta_static_ip`
- 值：`"192.168.1.300"`（第四段 300 > 255）
- 合法性依据：lwIP `ip4addr_aton()` 对 4 段形式检查 `val > 0xff` 则返回 0（`ip4_addr.c` case 4），因此 `app_parse_ipv4_str()` 失败，进入 `static_parse`。
- BLOB 内字节（abs `0x380..0x38F`，16B）：

```text
31 39 32 2e 31 36 38 2e 31 2e 33 30 30 00 00 00
```

- `sta_use_static`（abs `0x37F`）保持 `0x01`；gateway/mask/DNS 不变。

## 2. 离线镜像与哈希

| 文件 | SHA-256 |
| --- | --- |
| `nvs_page0_bad_ipfmt.bin`（4 KiB，拟写入 0x9000） | `5d05aa12eaee21653beecf0fe6004a703149e2851b6da173c223432dfa3587ca` |
| `nvs_bad_ipfmt.bin`（16 KiB，page1–3 与原始一致） | `40af6bee5ed2c9931e64339cfc9d0127fd9860902856ea3024f9e0a840eaaaa2` |
| 原始备份 `nvs_backup_1.bin` | `62cc574719585439635b1d6d1229d5c34e330c5c085cded78955e8fd02682389` |

结构校验（离线）：page state `0xFFFFFFFE`、seq 118、页头 CRC `0xAEE60426` 不变；bitmap 不变；`runtime_cfg` span=21、dataSize=640；blob CRC 与 entry CRC 均自洽；payload `use_static=1`、ip 为上述 16 字节。

## 3. 字节差异（page0）

- 允许区域：`0x144..0x147`（entry CRC）、`0x15C..0x15F`（blob CRC）、`0x380..0x38F`（ip）。
- 实际变化 12 字节：

```text
0x144..0x147  entry CRC     1323e2ed -> 556b1b0c（LE of 0x0C6B1B55）
0x15c..0x15f  blob dataCrc  99c91906 -> 18d26eff（LE of 0xFF6ED218）
0x38a..0x38a   '5' -> '3'
0x38c..0x38e  00 6f 40 -> 30 00 00
```

（新旧字符串共享 `192.168.1.` 前缀，故 ip 实际只变 5 字节。）

## 4. 写入前复核（任一不符即停）

| 项 | 期望 |
| --- | --- |
| MAC | `b0:a7:32:28:c8:e4` |
| 固件 | M1 正式 `1.2-a35eef9-dirty` |
| 当前 page0 SHA | `5ab2b53c0506cd006ba50a89298a65f640a483483a2103d4a2fc2e303d8946bb` |
| 当前 page1 SHA | `ebdb591398d38823925dfe750f7f081a430698936367032f66187929ca3cc88a` |
| 目标 page0 SHA | `5d05aa12eaee21653beecf0fe6004a703149e2851b6da173c223432dfa3587ca` |

## 5. 执行步骤（批准后）

1. `esptool --after no_reset write_flash 0x9000 nvs_page0_bad_ipfmt.bin`；
2. `--after no_reset read_flash 0x9000 0x1000`，与目标逐字节比对；不一致立即恢复、不启动；
3. 冷启动采集 ≥120s。

## 6. T4 断言（主判据）

| 断言 | 期望 |
| --- | --- |
| 静态应用 | `apply_begin mode=static source=boot` |
| 解析失败 | `apply_failed stage=static_parse err=ESP_ERR_INVALID_ARG` ×1 |
| DHCP 救援 | `recover action=dhcp_rescue result=ok state=fallback` ×1 |
| 状态 | `net_config_state=fallback` |
| 网络 | DHCP GOT_IP、MQTT connected |
| BLE 回读 | `sta_use_static=true`、`sta_static_ip="192.168.1.300"`，其余字段不变 |
| NVS | 启动后 page0 与坏镜像逐字节一致；page1 与原始一致 |
| 稳定性 | 无 abort/Guru/重启循环；无 192.168.43.x 回退 |

**已知观察项（不作为 T4 失败条件，必须记录）**：fallback 期间 `STA_CONNECTED` 可能仍打印 `static ip mode`，并对 DHCP 地址做静态 ARP 探测（T3 已确认原因）。T4 判据以 `apply_failed` / `dhcp_rescue` / `net_config_state` / 实际 DHCP+MQTT 为准。

## 7. 恢复

- 写回原始 page0，读回比对；
- 重启确认 `apply_begin mode=static`、`192.168.1.50`、MQTT；
- 若 page0 外任何变化，写回完整 16 KiB 备份。

## 8. 停止条件

写入前复核不符；读回不一致；启动无 `static_parse` 失败或出现 43.x 回退；abort/Guru/重启循环；NVS 被救援改写。任一发生即恢复并停止，不进入 T5。

## 9. 当前状态

T4 镜像已生成、校验完成；**未写设备、未重启**；C8E4 保持静态 `192.168.1.50`；65D8 保持 DHCP。
