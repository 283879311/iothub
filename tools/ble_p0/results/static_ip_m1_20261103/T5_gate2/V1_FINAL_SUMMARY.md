# T5 V1（RTC_NOINIT 软件复位保留）最终结果

| 阶段 | 内容 | 结果 |
| --- | --- | --- |
| 关口一 | hooks app 烧写；同会话 NVS A/B | PASS（A/B 均 bb3628c5…） |
| 阶段一 | 写 rtc_probe，不重启 | PASS（reboot_required=true，无自动重启） |
| 阶段二 | 软件复位 + 首次消费 | PASS（rst:0xc ×1，ticket_seen=1 probe=1 ×1，.50/MQTT） |
| 阶段三 | 第二次软件复位 | PASS（rst:0xc ×1，ticket_seen=0，.50/MQTT） |
| 阶段四 | 恢复正式 M1 | PASS（1.2-a35eef9-dirty，.50/MQTT，m1hook unknown_method） |

结论：RTC_NOINIT 票据在 esp_restart 软件复位下保留，且消费一次后不再触发。
V1 仅证明软件复位；V2（EN 硬复位）未执行，T5/T6 未执行。
