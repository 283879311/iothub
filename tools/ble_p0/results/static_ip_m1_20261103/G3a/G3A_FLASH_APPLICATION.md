# T5 V2 G3a 烧写 hooks 申请（待批准）

> 状态：**申请审查中；未执行任何设备操作**。
> 依据：`T5_V2_EN硬复位测试计划_r2.2.md`（SHA-256 `c2d3000994f5959a1a5a45728206543a4823ca257a14877dde1c4b85627ea76f`，G2 PASS）。
> 本申请只覆盖 G3a；G3b（probe + EN 测试复位）、G3c（恢复正式 M1）均未申请、未批准。

## 1. 授权范围

**仅 C8E4**（MAC `b0:a7:32:28:c8:e4`）；65D8 不连接、不烧写、不修改。

G3a 拟执行：

1. 通过 CP2102 进入 ROM 下载模式；
2. 读取 NVS 16 KiB 并只保留 SHA-256/长度（不保留原文）；
3. 仅向 app 分区 `0x10000` 写入 hooks BIN；
4. 同一 bootloader 会话内再次读取 NVS，比较 SHA-256，确认烧写未触碰 NVS；
5. 烧写后硬复位启动 hooks，采集 ≥60s 串口日志，确认版本、静态 `.50`、MQTT。

G3a **不包含**：

- 不写 `rtc_probe`/任何票据；
- 不做 G3b 的测试性 EN 复位；
- 不执行 BLE 配置/控制命令；
- 不写 NVS、不写 Wi-Fi/MQTT 配置、不擦 NVS；
- 不烧写 bootloader、分区表、otadata、phy_init、storage、coredump；
- 不恢复正式 M1（G3c 未批准）；
- 不执行 `erase-flash`、`erase-region`、`--erase-all`。

## 2. 目标 BIN（hooks）

| 项 | 值 |
| --- | --- |
| 路径 | `/tmp/m1_commit/build-hookstest/iothub.bin` |
| 大小 | 1,160,496 bytes |
| SHA-256 | `67913f191122ef85a180cee2f678b3631f64ba125c546d2c2978811ccc9c7f35` |
| ELF SHA-256 | `ee280e320fcf613423ddc4d77ffb3f20b64e30d02f5db2981c94b906c7c46df7` |
| Image | ESP32, 4MB, DIO, 40 MHz |
| App version | `1.2-afac9cb-dirty` |
| ESP-IDF | `v6.0.3-707-gdac489c6718` |

说明：该 BIN 的版本字符串为构建时旧值，但完整 SHA-256 与 V1 已验收 hooks 构建一致；二进制中存在 `boot_ticket_busy`、`reboot_scheduled`、`ticket_seen=1 invalid=1` 等字符串，说明包含后续修复。**以完整 SHA-256 为唯一选文件判据，不以版本字符串判据。**

执行前必须重新计算实际文件 SHA-256；若与上表不一致，停止，不重建、不替代、不烧写。

## 3. 烧写地址与分区

分区表 `partitions.csv`：

| 分区 | 偏移 | 大小 | 范围 |
| --- | --- | --- | --- |
| nvs | `0x9000` | `0x4000` | `0x9000–0xD000` |
| otadata | `0xD000` | `0x2000` | `0xD000–0xF000` |
| phy_init | `0xF000` | `0x1000` | `0xF000–0x10000` |
| ota_0 | `0x10000` | `0x1E0000` | `0x10000–0x1F0000` |
| ota_1 | `0x1F0000` | `0x1E0000` | — |

- 目标：`ota_0` 的 `0x10000`；BIN 结束约 `0x12B530`，完全位于 `ota_0` 内；
- NVS 位于 `0x9000–0xD000`，与 app 写入区无重叠；
- `otadata`、`phy_init` 不写；不改变启动槽；
- 前提：C8E4 当前由 `ota_0` 启动（与 V1 阶段四恢复后的状态一致）。若执行前不能确认，停止，不写 `otadata`。

## 4. NVS 保护

- 只允许 `write-flash 0x10000 <hooks.bin>`；禁止任何 NVS 地址写入；
- 禁止 `erase-flash`、`erase-region`、`write-flash --erase-all`；
- 禁止 `nvs_flash_erase`、BLE/串口配置写入、`diag.m1hook`；
- NVS 快照仅用于同一次 bootloader 会话内的“烧写前/后”SHA-256 对比，不作为跨启动字节一致性判据；
- NVS 可能含 Wi-Fi 凭据：快照文件权限 `600`，只计算 SHA-256 和长度，**不归档原文、不提交、不打印内容、不写入报告**，计算后立即删除；
- 若烧写前后 NVS SHA-256 不一致，停止，不启动 hooks，不写 NVS，不重试。

## 5. 拟执行命令（待批准后才执行）

变量：

```bash
PORT=/dev/ttyUSB1          # 执行前用 by-id 确认是 C8E4 的 CP2102
ESPTOOL=/home/tc/.espressif/python_env/idf6.0_py3.12_env/bin/esptool
HOOKS=/tmp/m1_commit/build-hookstest/iothub.bin
```

预检（宿主机，无设备）：

```bash
sha256sum "$HOOKS"
$ESPTOOL image-info "$HOOKS"
```

设备操作：

```bash
# 1) 进入下载模式并确认 MAC
$ESPTOOL --chip esp32 --port "$PORT" --before default-reset --after no-reset read-mac

# 2) NVS 烧写前快照（临时文件，权限 600；只保留哈希）
umask 077
NVS_BEFORE=$(mktemp /tmp/c8e4_nvs_before.XXXXXX)
$ESPTOOL --chip esp32 --port "$PORT" --before no-reset --after no-reset \
  read-flash 0x9000 0x4000 "$NVS_BEFORE"
sha256sum "$NVS_BEFORE" | tee /tmp/c8e4_nvs_before.sha256

# 3) 只写 app 0x10000，留在 bootloader
$ESPTOOL --chip esp32 --port "$PORT" --baud 460800 \
  --before no-reset --after no-reset \
  write-flash --flash-mode dio --flash-freq 40m --flash-size 4MB \
  0x10000 "$HOOKS"

# 4) 烧写后 NVS 快照（仍在 bootloader，app 尚未启动）
NVS_AFTER=$(mktemp /tmp/c8e4_nvs_after.XXXXXX)
$ESPTOOL --chip esp32 --port "$PORT" --before no-reset --after no-reset \
  read-flash 0x9000 0x4000 "$NVS_AFTER"
sha256sum "$NVS_AFTER" | tee /tmp/c8e4_nvs_after.sha256

# 5) 比较；必须一致
sha256sum "$NVS_BEFORE" "$NVS_AFTER"
rm -f "$NVS_BEFORE" "$NVS_AFTER"

# 6) 读 MAC 后硬复位，启动 hooks；随后采集串口 ≥60s
$ESPTOOL --chip esp32 --port "$PORT" --before no-reset --after hard-reset read-mac
```

说明：`--after hard-reset` 属于**烧写后启动复位**，不是 G3b 的测试性 EN 复位；本申请预计 2 次复位（下载模式进入 1 次、烧写后启动 1 次）。G3a 不使用 V2 票据判据，不要求 EN 波形；若批准方要求所有 EN 复位均有物理波形，需在 G3a 批准前补充仪器条件。

## 6. 验收标准

1. `read-mac` 返回 `b0:a7:32:28:c8:e4`；
2. `write-flash` 输出 `Hash of data verified`；
3. NVS 烧写前/后 SHA-256 完全一致；
4. 启动日志显示 hooks 构建（App version `1.2-afac9cb-dirty`），无 abort/Guru/重启循环；
5. 静态 IP `192.168.1.50` 保持，MQTT 在线；
6. 未调用 `diag.m1hook`，未写票据，未改任何配置。

以上 1–6 全部满足才判定 G3a 完成；否则停止并保留证据。

## 7. 中止与回退

| 条件 | 处理 |
| --- | --- |
| BIN SHA 不符/文件不存在 | 停止；不重建、不替代、不烧写 |
| MAC 不是 C8E4 | 停止；不烧写 |
| 无法进入下载模式 | 停止；不自行改用手动 BOOT/EN、不扩大授权 |
| `Hash of data verified` 缺失/失败 | 停止；不重试、不擦除 |
| NVS 前后 SHA 不一致 | 停止；不启动 hooks、不写 NVS、不重试 |
| hooks 启动失败/abort/循环 | 停止；保留日志；不烧正式 M1（G3c 未批准） |
| 静态 `.50` 或 MQTT 丢失 | 停止；不写配置；报告后另行申请 |
| 恢复需求超出 G3a | 停止；G3c 单独申请 |

## 8. 证据

- hooks BIN 的 SHA-256、大小、`image-info` 输出；
- `read-mac` 输出、烧写完整输出（含 `Hash of data verified`）；
- NVS before/after SHA-256 与长度（**不含 NVS 原文**）；
- 启动串口日志 ≥60s（含版本、静态 IP、MQTT；不含凭据/令牌）；
- 命令记录与时间戳；
- 所有证据文件 SHA-256。

## 9. 待确认项（不影响 G3a 本身）

r2.2 §3 指定正式恢复 BIN 为 `b0e6d6c7e1a5ba5271627d4e13aa0fc22b4b74c9517f7a11a7cab37e941ef5bf`。当前匹配文件为：

- `build/iothub.bin`，SHA `b0e6d6c7…`，App version `1.2-a35eef9-dirty`（与 V1 阶段四一致）。

另发现 `/tmp/m1_commit/build-formal/iothub.bin` 为 **`b824a464a33a69138009b6355ac2987147a033462df995f733a5be76516891c8`**，App version `1.2-659a399-dirty`，**不是** r2.2 指定的恢复镜像。G3c 申请前需确认使用 `b0e6…`，不得误用 `b824…`。
