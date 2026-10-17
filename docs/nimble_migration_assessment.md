# NimBLE 迁移可行性评估（预研 · 2026-10-16）

> 目的：判断把 BLE 主机栈从 Bluedroid 迁到 NimBLE 的可行性、收益、成本与风险，并给出
> **不动现有已验证 BLE 行为**的分阶段实施与回退方案。本文是评估与实施记录（代码已落地为默认 profile）。
> 相关条目：1.3-39/1.3-40（BLE 内存实测与裁剪）、1.3-44（全开稳态护栏）、
> `问题与版本优化.md` 3.3.2（NimBLE 吞吐调研）。

## 0. 结论（TL;DR）

| 项 | 结论 |
| --- | --- |
| **可行性** | ✅ **高**。全仓库 BLE 代码收敛在 3 个实现文件 + 3 个头文件 facade 后面；数据面/命令面/配置面 **0 行改动** |
| **预期收益** | BLE 全栈现吃 **~93KB 堆**（实测开/关差分）；迁移换掉 host 侧。**实测(§8 Step 4):稳态 free +9.1KB / largest +6.5KB,瞬态 largest 4.3KB→8.2KB;镜像 −93KB** |
| **必要性证据** | 板 B 本次抓包（全开 + MQTT 在线）：稳态 `free=16756`，**历史最低 `free=24` 字节** —— 系统长期在堆见底边缘运行且侥幸存活 |
| **成本** | 重写 ~1100 行的 3 个文件 → 预估 **4-6 人日** 实现 + 1-2 人日上机验收 |
| **风险** | 中。主要是行为回归（广播恢复/连接参数/通知背压），全部有现成 harness 可拦（见 §6） |
| **回退** | 三层、秒级：编译期双 profile（Bluedroid 代码原样保留）+ 双 OTA 槽 + NVS schema 不变（见 §5） |

**建议**：已按 §4 完成实施与验收；本文保留为设计依据与回退说明。

### 0.1 迁移完成状态（2026-10-17）

| 项 | 状态 |
| --- | --- |
| Step 1 双 profile 骨架 | ✅ 完成 |
| Step 2 peripheral(GATTS) | ✅ 完成；双板/手机数据面验证 |
| Step 3 central(GATTC) | ✅ 完成；双板 E2E（连接+双向 1:1，MTU 256，句柄 3/5） |
| Step 4 内存 A/B | ✅ 3 轮：稳态 free +9.1KB / largest +8.2KB；瞬态 largest 1.5KB→8.2KB |
| 默认 profile | ✅ NimBLE（2026-10-17，稳定性优先；15KB 判据被显式覆盖） |
| Step 5 收尾 | ✅ Bluedroid 代码保留作回退；文档已收口 |
| 长稳验证 | ✅ 1h BLE+MQTT；🔄 页面负载：pacing 后 5min largest 15.9KB/最差 2.7KB，1h 复测待会话隔离；24h 未做（1.3-66/67/70） |

- **回退**：`IDF_BUILD_DIR=build-bluedroid tools/idfbuild.sh build`（种子 `sdkconfig.bluedroid`）。
- **仍未单独验证（保留为后续项）**：OFF 模式 NimBLE host BSS 归还量、DLE/吞吐长压、
  GATT 已订阅位加固（§2 #14）、Bluedroid 回退镜像的 OTA/上机回归。

## 1. 现状与证据

### 1.1 代码面：facade 已把迁移面收敛到 3 个文件

| 文件 | 角色 | 栈 API 调用 | 迁移动作 |
| --- | --- | --- | --- |
| `main/bt.c`（416 行） | 栈生命周期 + GAP 广播 + OFF 内存归还 | esp_bt_controller_* / esp_bluedroid_* / esp_bt_mem_release / esp_ble_gap_* | **重写**（新文件，原文件不动） |
| `main/bt_gatts.c`（360 行） | NUS 外设：属性表 + 通知 + 写回调 | esp_ble_gatts_* / esp_ble_gatt_set_local_mtu / esp_ble_gap_update_conn_params | **重写** |
| `main/bt_gattc.c`（332 行） | NUS 主角色：扫描/连接/发现/订阅/写 | esp_ble_gattc_* / esp_ble_gap_scan_* | **重写** |
| `main/transport_ble.c` | 数据面接线（只调 facade） | **0** | 不动 |
| `main/ble_cmd.c` | 命令外壳（只 include `bt_gatts.h`） | **0** | 不动 |
| `main/data_pipe.c` / `status.c` / `config_apply.c` / `main.c` | 只经 facade 使用 BLE | **0** | 不动 |

对外的 facade 仅 3 个头文件：`bt.h`（7 个函数）、`bt_gatts.h`（6 个）、`bt_gattc.h`（4 个）。
⇒ 迁移是"换 3 个实现文件"，不是重写业务。

### 1.2 内存基线（实测，非推算）

- **1.3-40 实测**：`bt_role=off` free≈109.7KB ↔ `bt_role=ble` free≈16.7KB → **BLE 全栈 ≈93KB 堆**。
- **本次双板抓包（2026-10-16 02:30，被动读取 45s）**：
  - 板 B（全开：AP+STA+MQTT+BLE，MQTT 在线 ACK 计数在涨）：`heap free=16756 min=24 largest=9216`。
    **历史最低只剩 24 字节**——极端事件真实存在且已发生过，设备未崩（`CONFIG_BT_ABORT_WHEN_ALLOCATION_FAILS=n` 兜住了 BT 侧，见 1.3-44）。
  - 板 A：`free=34328 min=22676 largest=26624`（较轻载状态，固件版本早于板 B）。
- **静态段（map 实查）**：BT 全部静态存储仅 ~3.7KB（`_bt_bss` 428B + `_bt_data` 424B + `_bt_controller_bss` 2,716B + `_bt_controller_data` 248B）。
  ⇒ **93KB 几乎全是堆上运行时分配**，所以"裁 Kconfig"只能动零头（1.3-39 实测仅 2.2KB）。
- **可归因的宿主固定开销**（Bluedroid host 三个任务栈）：`BTC_TASK` 3072B + `BTU_TASK` 4352B + `hciT` 2048B+extra ≈ **10KB**；其余为 GATT 表/L2CAP/HCI 缓冲/OSI 框架/控制器池。

### 1.3 前提核实（IDF 6.0.2 源码实查）

| 事项 | 结论 |
| --- | --- |
| 两个 host 能否共存 | 否。`components/bt/Kconfig:9` 是 `choice BT_HOST`（Bluedroid / NimBLE / Controller-only 三选一）⇒ **必须双 build profile**，同一个镜像只能一个 host |
| OFF 模式内存归还 | `esp_bt_mem_release()` 实现在 **控制器层**（`controller/esp32/bt.c`）⇒ NimBLE 构建同样可用；NimBLE host 自身 BSS 是否被完整归还为**待验证项**（Phase 1 实测） |
| 栈初始化 | `nimble_port_init()` 内部已完成 `esp_bt_controller_init/enable(ESP_BT_MODE_BLE)` + `esp_nimble_hci_init()` + host 初始化（`nimble_port.c:173/304/310`）；宿主任务用 `nimble_port_freertos_init()` |
| 控制器配置 | 复用同一套 `CONFIG_BTDM_CTRL_*`（BLE-only、max_conn=1、adv 流控等全部保留生效） |
| GATTC 缓存 | NimBLE 侧有 **Espressif 扩展的 Bluedroid 形状缓存 API**（`ble_esp_gattc_cache.h`）：`ble_gattc_get_service / get_all_char / get_all_descr / get_descr_by_char_handle / get_attr_count / get_db`，结构体 `ble_gattc_char_elem_t` 等与 `esp_gattc_char_elem_t` 同形 ⇒ central 侧重写量显著缩小 |
| 参考例程（IDF 6.0.2 自带） | `examples/bluetooth/nimble/`：`bleprph`（广播+服务）、`blehr`（notify + `BLE_GAP_EVENT_SUBSCRIBE`）、`blecent`（发现/订阅/写）、`throughput_app`（吞吐基线） |

## 2. API 差异清单（逐条，NimBLE 名称均经 IDF 6.0.2 头文件核实）

| # | 功能 | 现用（Bluedroid） | 迁移后（NimBLE） | 差异要点 |
| --- | --- | --- | --- | --- |
| 1 | 栈启动 | `esp_bt_controller_init/enable` + `esp_bluedroid_init_with_cfg/enable` | `nimble_port_init()` + `nimble_port_freertos_init(host_task)` | NimBLE 一步到位（含控制器）；`nimble_port_run()` 在宿主任务内跑事件循环 |
| 2 | 栈拆除 | `esp_bluedroid_disable/deinit` + `esp_bt_controller_disable/deinit`（bt.c 已有官方逆序） | `nimble_port_stop()` + `nimble_port_deinit()` + 控制器同前 | 逆序语义一致 |
| 3 | OFF 内存归还 | `esp_bt_mem_release(ESP_BT_MODE_BTDM)` | **同一 API 不变**（控制器层实现） | NimBLE host BSS 覆盖范围待实测 |
| 4 | 设备名 | `esp_ble_gap_set_device_name` | `ble_gap_adv_set_fields()` 里 `name/name_len`（或在 GAP 服务里设 `GAP_DEVICE_NAME`） | 名字进广播包字段 |
| 5 | 广播数据 | `esp_ble_gap_config_adv_data(esp_ble_adv_data_t)` | `ble_gap_adv_set_fields(struct ble_hs_adv_fields)` | 字段一一对应：flags↔flags、include_name↔name、include_txpower↔tx_pwr_lvl+is_present；scan_rsp 两边都不发 |
| 6 | 广播启停 | `esp_ble_gap_start/stop_advertising` + `ADV_START_COMPLETE` 事件维护 `s_adv_active` | `ble_gap_adv_start(own_addr, NULL, BLE_HS_FOREVER, &adv_params, cb, arg)` / `ble_gap_adv_stop` / `ble_gap_adv_active` | **1.3-03 加固必须照搬**：断开后显式重开广播（NimBLE 同样不会自动恢复） |
| 7 | 连接参数请求 | `esp_ble_gap_update_conn_params(30-50ms, lat=0, tmo=6s)` | `ble_gap_update_params(conn_handle, struct ble_gap_upd_params)` | 回显走 `BLE_GAP_EVENT_CONN_UPDATE`（对应现状日志） |
| 8 | 本地 MTU | `esp_ble_gatt_set_local_mtu(517)` | `ble_att_set_preferred_mtu(517)`；协商结果读 `ble_att_mtu(conn)` | 分片算法（MTU-3）不变 |
| 9 | 服务定义 | `esp_gatts_attr_db_t` 属性表 + `create_attr_tab/start_service` | `struct ble_gatt_svc_def/chr_def/dsc_def` + `ble_gatts_count_cfg` + `ble_gatts_add_svcs` + `ble_gatts_start` | **NimBLE 自动生成 CCCD**（0x2902）——不再手写；属性/句柄编号会变（手机侧无感，调试日志会不同） |
| 10 | 写回调 | `ESP_GATTS_WRITE_EVT`（`param->write.value/len`） | 特征 `access_cb`（`BLE_GATT_ACCESS_OP_WRITE_CHR`，`os_mbuf`）→ `os_mbuf_copydata` | **回调形态不同**：从"事件+指针"变"逐特征回调+mbuf 取数"；"只入队"纪律保留 |
| 11 | 通知/分片 | `esp_ble_gatts_send_indicate(..., need_confirm=false)` | `ble_gatts_notify_custom(conn, handle, om)`（`om` 从 msys 取） | app 仍按 `ble_att_mtu()-3` 分片；mbuf 耗尽 = 新的失败模式（见 #12） |
| 12 | 发送拥塞 | `ESP_GATTS_CONGEST_EVT` → 暂停数据面 | `ble_gatts_notify_custom` 返回 `BLE_HS_EBUSY/ENOMEM` + `BLE_GAP_EVENT_NOTIFY_TX` 完成状态 | 背压语义由"事件"改为"返回码"；data_pipe 的 `dropped` 计数沿用 |
| 13 | 连接/断开 | `GATTS_CONNECT/DISCONNECT_EVT`（+ reason） | `BLE_GAP_EVENT_CONNECT / DISCONNECT`（统一回调） | 断开 reason 字段等价 |
| 14 | 订阅状态 | CCCD 写由栈处理；`app_ble_gatts_connected()` 只判连接 | `BLE_GAP_EVENT_SUBSCRIBE`（拿 subscribe/notify 位） | NimBLE 侧已记录 `s_subscribed`，但 notify 前仍未检查；保留为加固项 |
| 15 | 扫描 | `esp_ble_gap_set_scan_params/start_scanning` + `SCAN_RESULT` | `ble_gap_disc(own_addr, duration, &ble_gap_disc_params, cb, arg)` + `BLE_GAP_EVENT_DISC` / `ble_gap_disc_cancel` | 参数映射：active/passive、itvl/window（0.625ms 单位）、filter_duplicates |
| 16 | 发起连接 | `esp_ble_gattc_open(bda, addr_type, true)` | `ble_gap_connect(own_addr, &peer_addr, duration, &conn_params, cb, arg)` | peer_addr.type 直接来自扫描事件 |
| 17 | MTU 请求（主） | `esp_ble_gattc_send_mtu_req` | `ble_gattc_exchange_mtu(conn, cb, arg)` | 等价 |
| 18 | 服务/特征发现 | `esp_ble_gattc_search_service` + `get_all_char` + `get_descr_by_char_handle` | `ble_gattc_disc_all_svcs/disc_svc_by_uuid`（填充缓存）→ **`ble_gattc_get_service / get_all_char / get_descr_by_char_handle`（缓存扩展，Bluedroid 同形）** | 需开 `CONFIG_BT_NIMBLE_GATT_CACHING`；发现→缓存→按 UUID 取句柄的流程与现状一一对应 |
| 19 | 订阅对端通知 | `register_for_notify` + `write_char_descr(CCCD, 0x0001)` | `ble_gattc_write_flat(conn, cccd_handle, &val(2B), cb, arg)` | CCCD 句柄从 #18 的缓存取 |
| 20 | 写对端 | `esp_ble_gattc_write_char(NO_RSP)` | `ble_gattc_write_no_rsp_flat(conn, handle, data, len, cb, arg)` | 等价（NO_RSP 语义一致） |
| 21 | 收到通知 | `GATTC_NOTIFY_EVT` | `BLE_GAP_EVENT_NOTIFY_RX`（`os_mbuf`） | 取数用 `os_mbuf_copydata`；"只入队"纪律保留 |
| 22 | 广播名解析 | 手工解析 AD 结构（`app_ble_gattc_extract_name`） | **原样复用**（纯字节解析，与栈无关） | 0 改动 |
| 23 | 角色/状态查询 | `esp_bluedroid_get_status` 等 | `nimble_port_*` + 自维护状态（现状已有 `s_stack_started` 等） | bt.h 的 `app_bt_get_status` 语义不变 |

## 3. Kconfig 迁移对照（Bluedroid profile → NimBLE profile）

| 现配置 | NimBLE 侧 | 说明 |
| --- | --- | --- |
| `CONFIG_BT_BLUEDROID_ENABLED=y` | `=n` + `CONFIG_BT_NIMBLE_ENABLED=y` | `choice BT_HOST`，互斥 |
| `CONFIG_BT_ACL_CONNECTIONS=1` / `BTDM_CTRL_BLE_MAX_CONN=1` | `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1` + 控制器项保留 | 控制器层共用 |
| `CONFIG_BT_BLE_SMP_ENABLE=n` | `CONFIG_BT_NIMBLE_SECURITY_ENABLE=n` + `CONFIG_BT_NIMBLE_MAX_BONDS=0` | 项目本就无配对（CMD 壳是应用层 token 鉴权） |
| `BT_GATT_MAX_SR_PROFILES/ATTRIBUTES`（20） | 无对应项 | NimBLE 服务表按定义静态分配，规模由代码决定 |
| `BT_GATTC_MAX_CACHE_CHAR` / `NOTIF_REG_MAX` | `CONFIG_BT_NIMBLE_GATT_CACHING=y` + `GATT_CACHING_MAX_SVCS/MAX_CONNS`（+`INCLUDE_SERVICES` 可关） | 缓存容量改为服务级 |
| `BT_ALARM_MAX_NUM` / `BT_BLE_DYNAMIC_ENV_MEMORY` | 无对应项 | NimBLE 无 alarm 池、全动态 |
| `BT_BTC_TASK_STACK_SIZE`(3072) / `BTU`(4352) | `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE`（默认 4096） | **host 从三任务变一任务** |
| （新增） | `BT_NIMBLE_MSYS_1_BLOCK_COUNT/SIZE`（默认 12×256B）、`BT_NIMBLE_ATT_PREFERRED_MTU=517`、`BT_NIMBLE_ROLE_PERIPHERAL/CENTRAL/OBSERVER`、`BT_NIMBLE_LEGACY_VHCI_ENABLE`（esp32 默认 y） | 内存与角色开关 |
| 保留 | `BTDM_CTRL_*` 全套 + `BT_CLASSIC_ENABLED=n` + `BTDM_CTRL_MODE_BLE_ONLY=y` | 控制器配置不动 |

## 4. 分阶段方案（每阶段独立验收、随时可停）

**Phase 0 — 基线与护栏（不改 BLE 代码）**
- 双板升级到含 1.3-44 的固件（顺带完成 1.3-44 的上机验收），记录全开稳态快照：`free/min/largest/level`、`stack water`、`pipe_yield/shed`。
- 同时用 `tools/ble_nus/ble_nus.py`（bleak harness）跑 `scan/echo/soak` 建档，作为迁移对照基线。
- 判据：基线数据齐全 + 1.3-44 判据通过。

**Phase 1 — 双 profile 构建骨架（零功能改动）**
- 新增 `sdkconfig.nimble`（由 `SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.nimble"` 叠加），`tools/idfbuild.sh` 以独立目录出第二份镜像。**2026-10-17 起该 profile 已成为默认；Bluedroid 改为回退 profile（`sdkconfig.bluedroid` + `build-bluedroid`）。**
- 先只切 host、BLE 代码用 `#if CONFIG_BT_NIMBLE_ENABLED` 包空实现，验证：能编译、能启动、非 BLE 功能（WiFi/HTTP/MQTT/UART）全绿。
- 判据：Bluedroid 镜像与现网字节级等价（构建产物对比）；NimBLE 骨架镜像启动无 panic、WiFi/MQTT 正常。
- **待验证项在此解决**：NimBLE host BSS 是否被 `esp_bt_mem_release` 完整归还（OFF 模式收益）。

**Phase 2 — peripheral 迁移（GATTS + 广播）**
- 新文件 `bt_gatts_nimble.c` + `bt.c` 的 NimBLE 生命周期分支（`nimble_port_init/freertos_init/stop/deinit`）。
- 判据：`ble_nus.py scan/echo/soak` 全过（计数差值断言）；手机 nRF Connect 数据面 + CMD 四态（1.3-32 的 BLE 部分）；`adv_active`/重启广播（1.3-03）复验。

**Phase 3 — central 迁移**
- 新文件 `bt_gattc_nimble.c`（扫描→连接→缓存发现→订阅→写）。
- 判据：双板 E2E（1.3-02 回归：连接链 + 双向透传 1:1、dropped/overrun=0 + 改回从机不崩）；`data_pipe` 三边路由不变。

**Phase 4 — 内存 A/B 与决策**
- 同一 harness、同一场景（全开：BLE 连接 + MQTT 在线 + 页面连取）对比两份镜像的 `free/min/largest`。
- 决策判据：净省 ≥15KB 且 Phase 2/3 判据全绿 → 采纳 NimBLE 为默认；**2026-10-17 用户按稳定性优先覆盖该阈值,NimBLE 已设为默认**（实测净省 9.1KB/6.5KB，瞬态 largest 4.3KB→8.2KB）。

**Phase 5（可选）— 清收**
- NimBLE 胜出后：Bluedroid 实现文件移入 `#if !CONFIG_BT_NIMBLE_ENABLED` 或直接删除；文档收口。

## 5. 回退策略（三层，秒级）

| 层 | 机制 | 回退动作 | 恢复时间 |
| --- | --- | --- | --- |
| **代码** | **默认已是 NimBLE**；Bluedroid 完整种子保留在 `sdkconfig.bluedroid` | `IDF_BUILD_DIR=build-bluedroid tools/idfbuild.sh build` 重新构建回退镜像 | 一次构建 |
| **镜像** | 双 OTA 槽（ota_0/ota_1）+ 已启用 `BOOTLOADER_APP_ROLLBACK_ENABLE`（sdkconfig.defaults 已有） | NimBLE 镜像烧到**备用槽**；崩溃/未 mark-valid 自动回滚；手动=切回旧槽重启 | 秒级 |
| **数据** | NVS 配置 schema 不变（`bt_role/bt_device_name` 等）、NUS UUID 不变、MQTT/RPC 主题不变 | 无数据迁移；手机侧与云端无感 | — |

补充：`tools/idfbuild.sh` 已支持 profile 目录 ⇒ 默认 `build/`（NimBLE）与回退 `build-bluedroid/` 并存，随时 A/B 烧录。

## 6. 行为回归风险清单与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| 断开后广播不恢复（1.3-03 的实测坑） | 设备"消失" | Phase 2 显式照搬"断开即 `ble_gap_adv_start`"逻辑；`adv_active` 由 `BLE_GAP_EVENT_ADV_COMPLETE` 维护 |
| 通知背压语义变化（事件→返回码） | 数据面丢帧/积压 | `notify_custom` 返回 `EBUSY/ENOMEM` 即计入 `dropped`；msys 块数与 `BT_NIMBLE_MSYS_1_BLOCK_COUNT` 按压测调 |
| GATT 属性编号变化（自动 CCCD） | 仅调试日志不同 | 手机侧按 UUID 发现，无感；文档记录新句柄 |
| MTU 协商路径不同 | 分片大小 | 统一读 `ble_att_mtu()`；封顶 517（现状一致） |
| BT 日志 tag/格式变化 | 1.3-44 的 `stack water` 任务名、日志解析 | 监视表改为 `nimble_host` 任务名；本评估已列 |
| 角色切换（peripheral↔central） | 现状靠重启 | 不变（本就整机重启生效） |
| OFF 模式归还内存范围 | OFF 释放量 | Phase 1 待验证项；若不完整则保留 `esp_bt_mem_release` + `nimble_port_deinit` 组合实测 |
| 未知项（NimBLE 在 esp32 上的 DLE/吞吐行为） | 透传吞吐 | 3.3.2 已有调研（BLE 定位=低速旁路，≤0.75Mbps）；`ble_nus.py soak` 复测 |

## 7. 工作量与建议

| 工作项 | 规模 | 估时 |
| --- | --- | --- |
| `bt.c` NimBLE 生命周期分支 | ~150 行新代码 | 0.5-1 人日 |
| `bt_gatts_nimble.c`（NUS 服务 + 通知 + cmd 通道） | ~350 行 | 1.5-2 人日 |
| `bt_gattc_nimble.c`（扫描/连接/缓存发现/订阅/写） | ~300 行 | 1-1.5 人日 |
| sdkconfig.nimble + 构建脚本 profile | 配置 + 脚本 | 0.5 人日 |
| 上机验收（Phase 0/2/3/4） | 双板 + harness | 1-2 人日 |

**建议**：批准 Phase 0+1（低风险、零行为改动），拿到 NimBLE 骨架的内存数据后再决策 Phase 2/3 是否继续。所有 Bluedroid 代码与现网行为在本评估范围内**一行未动**。

---

## 8. 执行方案（2026-10-16 定稿，待实施）

> MCP 检索记录：Espressif TS 技能服务器本会话已离线（`ts_skill_list`/`ts_skill_reference_get` 均不可用），
> esp-pilot 目录无 nimble 技能。按技能规则改用**版本匹配的官方来源**：本地 IDF 6.0.2 源码与文档
> （本仓库实际编译依据），全部 API/Kconfig 名已在 §2/§3 逐条核对过，本节不重复。

### Step 0 — 基线快照（零代码）
- 两板当前 1.3-53 固件记最终基线：`free/min/largest`、`stack water`、BLE 数据面
  （central 侧 `NUS handles: rx=42 tx=44` + `peer notify enabled`）。
- `tools/ble_nus/ble_nus.py scan/echo/soak` 在**用户机器**跑一次存档（本机无 BlueZ，验收需用户侧配合）。

### Step 1 — 双 profile 骨架（零行为改动，先拿内存数据）
1. 新增 `sdkconfig.nimble`（`SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.nimble"` 叠加）：
   ```
   CONFIG_BT_NIMBLE_ENABLED=y
   CONFIG_BT_BLUEDROID_ENABLED=n
   CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
   CONFIG_BT_NIMBLE_SECURITY_ENABLE=n
   CONFIG_BT_NIMBLE_MAX_BONDS=0
   CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
   CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
   CONFIG_BT_NIMBLE_ROLE_OBSERVER=y
   CONFIG_BT_NIMBLE_GATT_CACHING=y
   CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=517
   CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096
   ```
   （BTDM_CTRL_* 控制器配置全套保留；Bluedroid 专属项在 NimBLE 构建下被 Kconfig 隐藏、自动失效）
2. 三个实现文件里 Bluedroid API 用 `#if CONFIG_BT_NIMBLE_ENABLED` 包成 stub（返回错误即可）——
   只保证 NimBLE profile 可编译链接、非 BLE 功能全绿；**默认 profile 仍是 Bluedroid**。
3. 构建：NimBLE 已是默认 `tools/idfbuild.sh build`；Bluedroid 回退用 `IDF_BUILD_DIR=build-bluedroid tools/idfbuild.sh build`。
4. **验收**：Bluedroid 镜像与现网字节等价（diff 产物）；NimBLE 骨架镜像启动无 panic、
   WiFi/MQTT/httpd 正常、bt_role=ble 下显示 `last_error` 而非崩溃。
5. **在此步实测 NimBLE 骨架的空载内存**（与 Step 0 基线比）——若骨架已显示明显省量，
   再投入 Step 2/3；数字不理想随时停，成本只有半天。

### Step 2 — peripheral 迁移（新文件 `bt_gatts_nimble.c`）
- NUS 服务表（4 特征、CCCD 由 NimBLE 自动生成）；`access_cb` 写事件只做
  memcpy+入队（ble_cmd_on_write / data_pipe_feed，纪律不变）；
- notify = `ble_gatts_notify_custom` + `os_mbuf`，按 `ble_att_mtu()-3` 分片；
- bt.c 的 NimBLE 生命周期分支：`nimble_port_init` + `nimble_port_freertos_init(host_task)`；
  广播 `ble_gap_adv_set_fields/adv_start`；**断开后显式重启广播（照搬 1.3-03）**；
  MTU `ble_att_set_preferred_mtu(517)`。
- facade（bt.h/bt_gatts.h）签名不变 ⇒ transport_ble/ble_cmd/data_pipe 零改动。
- **验收**：`ble_nus.py echo/soak` 全过（计数差值断言）；手机 nRF Connect 数据面 + CMD 四态；
  `adv_active`/断开恢复复验。

### Step 3 — central 迁移（新文件 `bt_gattc_nimble.c`）
- `ble_gap_disc`（主动扫描、名字解析复用现有纯字节函数）→ `ble_gap_connect` →
  `ble_gattc_disc_svc_by_uuid` 填缓存 → `ble_gattc_get_all_char/get_descr_by_char_handle`
  取句柄 → `ble_gattc_write_flat` 订阅 CCCD → `BLE_GAP_EVENT_NOTIFY_RX` 入队；
  写 = `ble_gattc_write_no_rsp_flat`。
- **验收**：双板 E2E（1.3-02 同判据：连接+双向透传 1:1、dropped/overrun=0、改回从机不崩）。

**状态(2026-10-17)**:代码完成并通过 NimBLE profile 编译;C8E4 板上 central 冒烟通过
(`central_scanning` + 扫描启动 + 无 panic,free 31.1KB/largest 26.6KB)。
双板 E2E（连接+双向透传 1:1）已于 2026-10-17 通过，详见 1.3版本优化.md 1.3-60。

### Step 4 — 内存 A/B 与决策
- 同一 harness、同一全开场景对比两份镜像：`free/min/largest`、`stack water`、
  `heap new low` 上下文。
- **决策判据**：净省 ≥15KB 且 Step 2/3 判据全绿 → NimBLE 升为默认 profile；**2026-10-17 用户按稳定性优先显式覆盖：NimBLE 已设为默认（实测 9.1KB/6.5KB，瞬态 largest 4.3→8.2KB）。**
  否则记录数据、保留 Bluedroid（回退零成本）。

### Step 4 实测结果(2026-10-17,单板 A/B 完成)

- **场景**:C8E4 同一配置(BLE peripheral + 手机 central 已连接 + MQTT 在线 + 每 2s 取一次
  `/config` 页面),60s/30 个样本;UART→BLE 路由临时置 0 排除 GPIO18 噪声;两块镜像同板同配置。
- **数据**(稳态样本 min/avg/max,字节):

  | 指标 | Bluedroid | NimBLE | Δ(NimBLE−Bluedroid) |
  | --- | --- | --- | --- |
  | free | 17,844 / 17,971 / 18,100 | 27,200 / 27,345 / 27,604 | **+9,356 / +9,374 / +9,504** |
  | largest | 11,776(恒定) | 18,432(恒定) | **+6,656** |
  | 启动期最小 free(diag min) | 1,512 | 7,076 | +5,564 |
  | 采样期瞬态低点 | 5,560 free / 4,352 largest(t≈27s) | 11,764 free / 8,192 largest(t≈9s) | +6,204 / +3,840 |
  | 镜像大小 | 1,353,136 | 1,259,648 | **−93,488** |
  | BT 任务 | BTC_TASK/hciT 常驻 | 无(BTC/hciT 消失) | 少 2-3 个任务 |

- **判据对照与最终决策**:实测净省 **9.1KB free / 6.5KB largest**,低于 15KB 判据。
  2026-10-17 用户按**稳定性优先**明确决策:**NimBLE 设为默认 profile**(15KB 判据被显式覆盖)。
  依据:全开稳态 free/largest 分别 +9.1KB/+6.5KB,采样期最差 largest 4.3KB→8.2KB,
  启动期最低 free 1.5KB→7.1KB,镜像 −93KB、少 2-3 个任务。
- **Bluedroid 回退(保留)**:`sdkconfig.bluedroid` 为完整种子(含 ACL=1/SMP 关/GATT profiles=4),
  构建命令 `IDF_BUILD_DIR=build-bluedroid tools/idfbuild.sh build`;根 `sdkconfig` 默认 NimBLE。
- **复现**:`/tmp/step4_measure.sh bluedroid|nimble`(或按同场景手工采样),原始样本在
  `/tmp/step4_*_samples.csv`、串口日志在 `/tmp/step4_*_serial.log`。
- **3 轮复测(2026-10-17,每 profile 90 样本)**:稳态 free 17,748–18,248(Bluedroid) vs
  26,884–27,616(NimBLE),largest 10,240–11,776 vs 18,432–19,456;最差瞬态 largest
  **1,536 vs 8,192**;Bluedroid 有 **2/3** 轮出现 `largest < 2048`(503 阈值),NimBLE **0/3**;
  90/90 样本均 BLE+MQTT 在线。瞬态优势可重复,维持 NimBLE 默认。


### Step 5 — 收尾 ✅ 已完成（Bluedroid 代码保留作回退）
- NimBLE 已是默认；Bluedroid 实现文件保持 `#if !CONFIG_BT_NIMBLE_ENABLED` 并保留为回退 profile，不删除。
- 文档收口：README、本评估、`1.3版本优化.md` 已同步。

### 回退（三层，秒级）
1. 代码：默认 NimBLE；回退用 `sdkconfig.bluedroid` + `build-bluedroid`（根配置构建后自动恢复）；
2. 镜像：双 OTA 槽 + `BOOTLOADER_APP_ROLLBACK_ENABLE`（已启用），NimBLE 固件进备用槽，崩溃自动回滚；
3. 数据：NVS schema / NUS UUID / MQTT 主题均不变，手机与云端无感。

### 风险与对策（执行时逐条对照）
| 风险 | 对策 |
| --- | --- |
| 断开后广播不恢复（1.3-03 实测坑） | Step 2 显式照搬"断开即 adv_start"；`adv_active` 由 ADV_COMPLETE 事件维护 |
| notify 背压从"事件"变"返回码" | `EBUSY/ENOMEM` 计入 dropped；msys 块数按压测调（`BT_NIMBLE_MSYS_1_BLOCK_COUNT`） |
| GATT 句柄编号变化 | 手机按 UUID 发现，无感；调试日志对新句柄 |
| 角色切换 | 不变（整机重启生效） |
| OFF 模式归还范围 | Step 1 验证 `esp_bt_mem_release` + `nimble_port_deinit` 组合的归还量 |
