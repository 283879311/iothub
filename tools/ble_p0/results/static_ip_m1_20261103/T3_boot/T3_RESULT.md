# T3：静态字段缺失 → 启动 DHCP 救援（C8E4）

- 日期：2026-11-03
- 设备：C8E4，MAC b0:a7:32:28:c8:e4，/dev/ttyUSB1
- 固件：M1 正式 1.2-a35eef9-dirty
- 故障镜像：NVS page0 语义无效（sta_use_static=1、ip 为空），NVS 结构与 CRC 有效
- 过程：写 page0 4KiB → 读回一致 → 冷启动采集 125s → NVS 回读 → 恢复原始 page0 → 重启确认

## 写入与读回

- 写入 4096B @0x9000，esptool "Hash of data verified"；
- 读回 SHA `50944dab…6c1059`，与 nvs_page0_bad.bin 逐字节一致。

## T3 断言（PASS）

| 断言 | 期望 | 实测 |
| --- | --- | --- |
| 静态应用 | `apply_begin mode=static source=boot` | 1 次 |
| 解析失败 | `apply_failed stage=static_parse` | 1 次（err=ESP_ERR_INVALID_ARG，attempt=1） |
| DHCP 救援 | `recover action=dhcp_rescue result=ok state=fallback` | 1 次 |
| 状态 | `net_config_state=fallback` | 1 次 |
| 网络 | DHCP GOT_IP | 192.168.1.28 |
| MQTT | connected | 1 次 |
| BLE 回读 | `sta_use_static=true`、`sta_static_ip=""` | 一致；网关/掩码/DNS 保持原值 |
| `network.status` | `sta_ip` 为 DHCP 地址 | 192.168.1.28，connected=true |
| 异常 | 无 abort/Guru/重启循环 | 0；仅 1 次 POWERON_RESET |
| 旧配置回退 | 不得出现 192.168.43.x | 0 |

## NVS 硬证据（救援未改写）

| 页 | 启动后 SHA-256 | 对比 |
| --- | --- | --- |
| page0 | `50944dab89a55304b19a91d17c3a31e07dd4b9bbfe7ba19837c2224a0d6c1059` | 与坏镜像逐字节一致 |
| page1 | `ebdb591398d38823925dfe750f7f081a430698936367032f66187929ca3cc88a` | 与原始备份一致 |

## 观察项（不改变 T3 判定，但需记录）

fallback 期间日志出现：

```text
STA_CONNECTED: static ip mode
static ip 192.168.1.28: no ARP answer, no conflict
```

原因：`STA_CONNECTED` / `GOT_IP` 处理器读取的是**持久化** `s_config.sta_use_static=true`，而运行 netif 已被救援切换到 DHCP。结果：
- 日志把 fallback 误标为 static ip mode；
- 对 DHCP 地址执行了静态冲突 ARP 探测（本次结果 no conflict）。

T3 的验收条目仍满足（DHCP 地址、MQTT、NVS 不变），但这是"配置模式与运行时模式分裂"的可观察缺陷，建议后续修正为按运行态 `cfg_state` 判断；不阻塞 T3 结论，需在报告中保留。

## 恢复结果

- 写回原始 page0（4096B），读回 SHA `5ab2b53c…d8946bb`，与原始一致；
- 重启：`apply_begin mode=static`、`static ip configured: 192.168.1.50`、GOT_IP `192.168.1.50`、MQTT connected；
- C8E4 已恢复静态 `192.168.1.50`。

## 结论

T3 PASS（按既定验收条目）；附带上述观察项，未执行 T4。
