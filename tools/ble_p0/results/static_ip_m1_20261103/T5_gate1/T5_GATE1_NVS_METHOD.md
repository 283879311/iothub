# V1 关口一：NVS 校验方法修正（未烧写）

## 现象

- 新基线 `2a292548…`（T5_gate1/nvs_before_flash_20261103.bin）；
- 仅做 esptool 读取/复位后，再次读取为 `19de98c4…`，NVS 16 KiB 再次变化；
- 逻辑配置始终为静态 `192.168.1.50`、MQTT connected，无 43.x；
- 每次 esptool 操作都会硬复位 → 应用启动 → NVS 页状态/内容变化。

## 根因（已证实/推断）

- **已证实**：每次启动后 NVS 原始 16 KiB 变化；连续两次读取（无启动间隔）一致。
- 启动日志显示 `wifi:config NVS flash: enabled`；NVS 中存在 `nvs.net80211`、`phy` 等命名空间，符合 Wi-Fi 栈在启动时写 NVS 的行为。
- 因此**跨启动的 16 KiB 对比不成立**：变化来自启动期 Wi-Fi/NVS 写入，不是配置修改，也不是烧写。

## 修正后的关口一 NVS 判据（同一 bootloader 会话）

不再跨启动比较。用 esptool 保持 bootloader（`--after no_reset`），在**同一次 bootloader 会话**内完成：

1. `read_flash 0x9000 0x4000 nvs_A`（`--after no_reset`，不进 app）；
2. `write_flash 0x10000 <hooks bin>`（`--after no_reset`）；
3. `read_flash 0x9000 0x4000 nvs_B`（`--after no_reset`）；
4. `cmp nvs_A nvs_B` + SHA-256 比较，要求完全一致；
5. 再做硬复位，进入启动/功能/BLE 验证。

这样 NVS 对比只覆盖“烧写 app”这一动作，排除启动期 Wi-Fi 写 NVS 的干扰。

## 边界

- 不写回 T3 备份、不擦 NVS、不改配置；
- T3 原始基线继续保留；
- 本方法只调整关口一的 NVS 对比方式，不改变烧写范围（仅 app 0x10000）。
