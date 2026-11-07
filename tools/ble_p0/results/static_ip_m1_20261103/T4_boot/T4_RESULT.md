# T4：静态字段格式非法 → 启动 DHCP 救援（C8E4）

- 日期：2026-11-03
- 设备：C8E4，MAC b0:a7:32:28:c8:e4，/dev/ttyUSB1
- 固件：M1 正式 1.2-a35eef9-dirty
- 故障构造：`sta_static_ip="192.168.1.300"`（IPv4 第四段 300>255），`sta_use_static=true`，NVS 结构与 CRC 有效
- 过程：写 page0 4KiB → 读回一致 → 冷启动 125s → BLE 回读 → NVS 回读 → 恢复原始 page0 → 重启确认

## 写入与读回

- 写入 4096B @0x9000，esptool "Hash of data verified"；
- 读回 SHA `5d05aa12…fa3587ca`，与 `nvs_page0_bad_ipfmt.bin` 逐字节一致。

## T4 断言（PASS）

| 断言 | 期望 | 实测 |
| --- | --- | --- |
| 静态应用 | `apply_begin mode=static source=boot` | 1 次 |
| 解析失败 | `apply_failed stage=static_parse err=ESP_ERR_INVALID_ARG` | 1 次（mode=static attempt=1） |
| DHCP 救援 | `recover action=dhcp_rescue result=ok state=fallback` | 1 次 |
| 状态 | `net_config_state=fallback` | 1 次 |
| 网络 | DHCP GOT_IP | 192.168.1.28 |
| MQTT | connected | 1 次 |
| BLE 回读 | `sta_use_static=true`、`sta_static_ip="192.168.1.300"` | 一致；其余字段不变 |
| network.status | DHCP 地址 | 192.168.1.28，connected=true，mqtt_connected=true |
| 异常 | 无 abort/Guru/重启循环 | 0；仅 1 次 POWERON_RESET |
| 旧配置回退 | 不得出现 192.168.43.x | 0 |

## NVS 硬证据

| 页 | 启动后 SHA-256 | 对比 |
| --- | --- | --- |
| page0 | `5d05aa12eaee21653beecf0fe6004a703149e2851b6da173c223432dfa3587ca` | 与坏镜像逐字节一致 |
| page1 | `ebdb591398d38823925dfe750f7f081a430698936367032f66187929ca3cc88a` | 与原始备份一致 |

## 已知观察项（不作为失败条件，已记录）

fallback 期间仍出现：

```text
STA_CONNECTED: static ip mode
static ip 192.168.1.28: no ARP answer, no conflict
```

原因同 T3：处理器读取持久化 `sta_use_static=true`，而运行 netif 已切 DHCP。T4 判据以 `apply_failed`/`dhcp_rescue`/`net_config_state`/实际 DHCP+MQTT 为准。

## 恢复结果

- 写回原始 page0，读回 SHA `5ab2b53c…d8946bb`，与原始一致；
- 重启：`apply_begin mode=static`、`static ip configured: 192.168.1.50`、GOT_IP `.50`、MQTT connected；
- C8E4 已恢复静态 `192.168.1.50`。

## 结论

T4 PASS（按既定验收条目）；未执行 T5，未改固件。
