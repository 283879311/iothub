# T3 设备写入审批卡（未执行）

> 当前状态：等待单独批准。未写 page0、未重启、未连接设备。
> 范围：仅 C8E4（`/dev/ttyUSB1`）NVS page0 4 KiB；65D8 不参与。

## 1. 写入前复核（任一不符即停止）

| 项 | 期望 |
| --- | --- |
| MAC | `b0:a7:32:28:c8:e4` |
| 当前固件 | M1 正式，`App version: 1.2-a35eef9-dirty` |
| 当前 page0 读回 SHA-256 | `5ab2b53c0506cd006ba50a89298a65f640a483483a2103d4a2fc2e303d8946bb` |
| 当前 page1 读回 SHA-256 | `ebdb591398d38823925dfe750f7f081a430698936367032f66187929ca3cc88a` |
| 目标镜像 SHA-256 | `50944dab89a55304b19a91d17c3a31e07dd4b9bbfe7ba19837c2224a0d6c1059` |
| 原始备份 SHA-256 | `62cc5747…682389`（16 KiB） |

## 2. 写入与读回（批准后执行）

```bash
esptool.py -p /dev/ttyUSB1 write_flash 0x9000 \
  tools/ble_p0/results/static_ip_m1_20261103/T3_nvs_backup/nvs_page0_bad.bin

esptool.py -p /dev/ttyUSB1 read_flash 0x9000 0x1000 /tmp/t3_p0_readback.bin
cmp /tmp/t3_p0_readback.bin .../nvs_page0_bad.bin   # 必须一致
```

- 只擦写 0x9000–0x9FFF（page0 所在 4 KiB 扇区）；page1–3 在其他扇区，不写。
- 读回不一致：立即写回原始 page0，不启动设备，停止 T3。

## 3. 冷启动 T3 断言（≥120s）

| 断言 | 期望 |
| --- | --- |
| 启动 | `apply_begin mode=static source=boot` |
| 解析失败 | `apply_failed stage=static_parse` 仅 1 次 |
| 救援 | `recover action=dhcp_rescue result=ok state=fallback` |
| 状态 | `net_config_state=fallback` |
| 网络 | DHCP GOT_IP（192.168.1.x）、MQTT connected |
| BLE | 可登录，`config.get` 可读 |
| 异常 | 无 abort/Guru/重启循环 |

## 4. NVS 未被救援改写（硬证据）

- 启动后再次 `read_flash 0x9000 0x1000`，与 `nvs_page0_bad.bin` 逐字节比对，必须**完全一致**；
- 同时读回 page1，与原始 page1 比对，必须一致；
- 若 page0 出现任何额外字节变化 → 判 M1-07 失败，恢复完整 16 KiB 备份。

## 5. BLE 回读（软证据）

- `config.get`：`sta_use_static=true`、`sta_static_ip=""`、gateway/mask/dns 保持原值；
- `network.status`：`sta_ip` 为 DHCP 地址（非 192.168.1.50）。

## 6. 恢复（T3 完成后立即执行）

1. 写回原始 page0（`nvs_backup_1.bin[0:4096]`）；
2. 读回比对；
3. 重启，确认 `apply_begin mode=static`、`192.168.1.50`、MQTT 在线；
4. 若发现 page0 之外有任何变化，写回完整 16 KiB 备份。

## 7. 停止条件

- 写入前任一复核不符；
- 写入后读回不一致；
- 启动出现 192.168.43.x/无 `static_parse` 失败（说明回退 page1）；
- abort / Guru / 重启循环；
- NVS 被救援改写。

以上任一发生：停止 T3，恢复备份，保留日志，不继续 T4。

## 8. 当前状态

未批准写入；C8E4 保持静态 `192.168.1.50`；65D8 保持 DHCP。
