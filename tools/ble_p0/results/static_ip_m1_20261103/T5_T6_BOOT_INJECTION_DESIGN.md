# T5/T6 启动期故障注入方案设计（未实施）

> 目标：在**冷启动**阶段注入 `static_set_ip` / `static_dhcpc_stop` 失败，验证 M1 的启动救援链。
> 约束：不改 M1 功能行为、不改 tryconnect、正式构建零测试代码、不污染业务 NVS、可恢复。

## 1. 候选方案对比

| 方案 | 机制 | 跨冷启动 | 一次性 | NVS 写入 | 正式构建清洁性 | 结论 |
| --- | --- | --- | --- | --- | --- | --- |
| A. 编译期固定注入 | 专用构建 `-DAPP_WIFI_BOOT_FAIL_STAGE=<id>`，启动时命中一次 | 是（固化在固件） | 每次上电都注入 | 无 | 宏不定义即无代码 | 简单但"自动清除"需重刷正式固件 |
| B. NVS 测试键 | hooks 构建读独立 namespace 键，消费后删除 | 是 | 是 | 有（测试 namespace；可能触发 NVS 页 GC，干扰 M1-07 原始页对比） | 默认 OFF 无代码 | 与 T3/T4 的"NVS 未被改写"硬证据冲突 |
| C. RTC_NOINIT 一次性票据 | 运行动作写 RTC 结构；启动时消费并清零 | 是（RTC 域在软复位保留） | 是（消费即清零） | **无** | 默认 OFF 无代码 | **推荐**，但需先验证本板 RTC 在 esptool 复位下保留 |

推荐 **C**；若 RTC 保留性验证失败，退回 **A**。

## 2. 方案 C 设计

### 2.1 数据与生命周期

```c
typedef struct {
    uint32_t magic;    /* 'M1BF' */
    uint16_t version;  /* 1 */
    uint16_t stage;    /* APP_WIFI_APPLY_STAGE_* */
    uint8_t  count;    /* 1 = 本次启动只注入一次 */
    uint8_t  reserved[3];
    uint32_t crc;      /* crc32(magic..reserved) */
} app_wifi_boot_fail_ticket_t;
RTC_NOINIT_ATTR static app_wifi_boot_fail_ticket_t s_boot_fail_ticket;
```

- **写入**（仅 hooks 构建，运行中）：`diag.m1hook action=boot_fail stage=static_set_ip count=1` → 校验 magic/CRC 后写入 RTC，返回 `reboot_required`。
- **消费**（启动最早、apply 前）：`app_wifi_test_boot_arm_from_rtc()`：
  1. 校验 magic/version/CRC；
  2. 合法则把 stage/count 复制到 RAM 注入状态；
  3. **立即清零 RTC 票据**（一次性；断电/重启不会二次注入）。
- **失败注入点**：复用 `app_wifi_test_take_failure(stage)`；`count=1` 只命中候选 apply，LKG/救援的第二次 apply 不再命中。
- **自动清除**：消费即清零；CRC/magic 不合法时忽略并清零；上电（power-on）RTC 不保留。
- **避免误注入**：正式构建不编译该代码；hooks 构建也必须 magic+CRC 匹配；仅 `APP_WIFI_FAULT_INJECT` 下存在。

### 2.2 断电/复位行为

| 时机 | 结果 |
| --- | --- |
| 写入票据后、复位前断电 | RTC 丢失 → 本次不注入（测试需重写票据） |
| 复位后、apply 前断电 | RTC 丢失 → 不注入 |
| 票据被消费后 | QCR 清零 → 后续启动不注入 |
| 恢复正式固件 | 无注入代码 |

### 2.3 构建与隔离

- `APP_WIFI_FAULT_INJECT=ON` 的 hooks 构建才包含 RTC 票据与 `boot_fail` 动作；
- 正式构建（默认 OFF）：无结构、无动作、无符号；沿用 B0 符号扫描验证；
- 不改 M1 功能路径；注入仅在 `take_failure` 前多一次 boot 票据 seed。

## 3. T5/T6 用例设计

| 用例 | 票据 stage | 预期日志 |
| --- | --- | --- |
| T5 | `static_set_ip` | `apply_failed stage=static_set_ip err=ESP_FAIL mode=static` → `dhcp_rescue result=ok state=fallback` |
| T6 | `static_dhcpc_stop` | `apply_failed stage=static_dhcpc_stop err=ESP_FAIL mode=static` → rescue fallback |

共同断言：`apply_begin mode=static`、失败仅 1 次、救援 1 次、`net_config_state=fallback`、DHCP GOT_IP、MQTT、NVS page0/page1 与注入前一致（票据在 RTC，不在 NVS）、无 abort/Guru/重启循环、BLE 回读静态字段仍为 .50。

## 4. 执行与恢复

1. 刷 hooks 构建到 C8E4（正式 M1 备份保留）；
2. 运行态写入票据（`diag.m1hook boot_fail ...`）；
3. 软复位（esptool hard reset），采集 ≥120s；
4. 断言；NVS 原始页对比；
5. 恢复：重刷 M1 正式构建 `b0e6d6c7…`，重启确认静态 `.50` + MQTT。

## 5. 先决验证（T5 前置，不改功能）

在 hooks 构建中先做**无害**验证：
- 写票据 `stage=wifi_start count=0`（或专用 `probe` 动作），复位后读取 RTC 打印 `ticket_seen=1/0`；
- 确认 esptool `hard reset` 下 RTC_NOINIT 保留；
- 若不保留，切方案 A（编译期固定 stage），T5/T6 各自构建一次。

## 6. 风险与边界

- RTC_NOINIT 保留性依赖复位类型；必须先验证，不能假设；
- hooks 构建仅用于测试设备，不进入正式发布；
- 不写业务 NVS；票据不落 NVS，避免干扰 M1-07 的原始页对比；
- 不改 tryconnect 状态机、不改 M1 恢复逻辑；
- 本文件仅为设计，未写代码、未烧板、未执行 T5/T6。

---

## 7. 评审裁决补充（2026-11-03）

### 7.1 票据消费顺序（固定）

1. 读取 RTC 票据到局部变量；
2. 校验 magic/version/stage/count/CRC；
3. **先清除 RTC 持久票据**；
4. 再把有效票据复制到 RAM 注入状态；
5. 进入正常启动 apply。

取舍：清除后、RAM 建立前若复位，注入丢失但不会重复；对一次性注入更安全。

### 7.2 票据结构与校验

- `magic = 'M1BF'`；`version = 1`；未知版本拒绝；
- `stage` 仅允许 `STATIC_DHCPC_STOP`（T6）与 `STATIC_SET_IP`（T5）；其余拒绝；
- `count` 仅允许 `1`；0 或越界拒绝；
- `CRC` 使用固定字节序（LE）覆盖 `magic(4)+version(2)+stage(2)+count(1)+reserved(3)` 共 12 字节，**不对含 padding 的结构体直接计算**；
- 无效票据：忽略并清除，不影响正常启动；
- 消费成功：清除 RTC 票据，救援 apply 不再命中。

### 7.3 复位类型验证（硬性前置）

| 项 | 操作 | 预期 |
| --- | --- | --- |
| V1 软件复位 | 写 probe 票据 → `esp_restart()` | `ticket_seen=1 probe=1` |
| V2 EN 硬复位 | 写 probe 票据 → esptool `hard reset` | `ticket_seen=1 probe=1` |
| 断电上电（观察） | 写 probe 票据 → 断电再上电 | 允许票据丢失，记录边界 |

V1/V2 分别记录，不得互相替代；T5/T6 使用哪种复位就验证哪种。

### 7.4 注入优先级

- 启动票据在 boot 最早消费并 seed RAM 注入状态；
- 运行期 `apply_fail` 在已有未消费注入时返回 busy（`ESP_ERR_INVALID_STATE`），不覆盖；
- `count=1` 只命中候选 apply；LKG/救援的第二次 apply 不再命中。

### 7.5 日志

- `ticket_seen=1 stage=... count=1 injected=1`；
- probe：`ticket_seen=1 probe=1`；
- 无效：`ticket_seen=1 invalid=1`；
- 不输出 CRC/内部字节。

### 7.6 正式构建隔离

- RTC 结构、写入命令、消费逻辑全部在 `APP_WIFI_FAULT_INJECT` 内；
- `boot_fail`/`rtc_probe` 仅 BLE 来源且需 token；
- 禁止 MQTT 注入；
- 正式构建符号扫描为空；重刷正式构建后确认静态 `.50` + MQTT。
