# V1 关口一：烧写前复核停止（NVS 基线不一致）

> 未烧写、未写 NVS、未执行 probe。设备保持 M1 正式 + 静态 192.168.1.50。

## 复核结果

| 项 | 期望 | 实际 | 结论 |
| --- | --- | --- | --- |
| MAC | b0:a7:32:28:c8:e4 | 同 | PASS |
| 固件 | 1.2-a35eef9-dirty | 同 | PASS |
| 目标 BIN | 67913f19… | 同 | PASS |
| 逻辑配置 | static .50 + MQTT | 同（BLE 回读） | PASS |
| NVS 16 KiB | 62cc5747…（T3 备份） | **2a292548…** | **FAIL** |
| page0 | 5ab2b53c… | 58c4a85d… | FAIL |
| page1 | ebdb5913… | ebdb5913… | PASS |

## 差异性质

- NVS 发生**页重组/压缩**：page0/1 state=`0xFFFFFFFC`（FULL），page2 已擦除，page3 state=`0xFFFFFFFE`（ACTIVE）seq=119；
- 逻辑配置未变：`sta_use_static=true`、`192.168.1.50`、gateway/mask/DNS 一致、MQTT connected；旧 43.x 不存在；
- 当前 16 KiB 连续两次读取完全一致（`2a292548…`），状态稳定。

## 结论

T3 的字节级基线只在“写回备份后立即读取”时成立；正常启动后的 NVS GC/整理会改写页布局（配置内容不变）。
关口一的“烧写前后 NVS 逐字节一致”应使用**本次烧写前的新快照**作为对照，而不是 T3 历史备份。

## 待裁决

1. 接受以 `2a292548…`（`nvs_before_flash_20261103.bin`）作为 V1 关口一的新基线，继续关口一；
2. 或先调查 NVS 重组触发源（只读）后再决定；
3. 当前未烧写，C8E4 保持正式固件 + 静态 .50。
