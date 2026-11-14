# `cmd.c` 第一轮逐函数审查（2026-11-14）

> 范围：基于审查时 `main/cmd.c` 的静态阅读，覆盖函数职责、主要调用关系、重复逻辑、锁使用和拆分建议。本文是架构审查记录，不等同于并发正确性证明；未在本轮执行编译、静态分析器或真机复现。
>
> 审查基线：分支 `main`，文件 `main/cmd.c`。后续若代码变化，应以新提交重新核对行号和调用关系。

## 1. 总体结论

`cmd.c` 同时承担统一命令入口、策略检查、配置事务、网络异步 operation、设备控制和状态 JSON 构造。主要问题不是单纯文件过长，而是**协议分发、业务编排、跨任务状态和响应构造耦合在一起**。

建议保留现有统一命令语义和网络事务保护机制，以小步、可回滚方式抽离模块。第一阶段不改变网络事务算法、状态迁移、锁类型、generation 校验、cleanup 顺序和协议字段。

## 2. 函数职责分组

### 2.1 网络异步事务与生命周期

| 函数 | 职责 | 处理建议 |
|---|---|---|
| `app_cmd_net_stage_snap_put/get` | 按 operation ID 保存/读取阶段快照 | 保留在网络事务模块 |
| `app_cmd_take_accepted_ttl` | 取得 accepted operation 的 TTL | 保留；确认 TTL 配置来源及边界 |
| `app_cmd_net_job_lock/unlock` | 保护网络 job/trial 共享状态 | 保留递归锁语义 |
| `app_cmd_net_async_clear_job` | 清理当前 job 状态 | 保留，限定锁前置条件 |
| `app_cmd_net_async_clear_job_sync` | 提供自行加锁的同步清理入口 | 暂留；迁移后评估是否可合并为一个明确 API |
| `app_cmd_static_switch_blocked` | 校验静态网络切换约束 | 保留，归网络配置/事务内部 |
| `app_cmd_net_store_trial` | 保存试连候选、owner、operation 及事务关联数据 | 保留 |
| `app_cmd_net_trial_clear` | 清理 trial 与相关配置事务状态 | 保留 |
| `app_cmd_net_on_ble_disconnect` | BLE 断连时处理 BLE owner 的网络事务 | 保留，需验证取消与 cleanup 竞争 |
| `app_cmd_scan_gate_check/stamp` | 扫描节流检查和时间戳更新 | 保留，归普通网络命令或网络协调模块 |
| `app_cmd_net_op_active` | 对外查询网络 operation 是否活动 | 保留为窄接口，不暴露内部结构 |
| `app_cmd_net_trial_ready` | 检查 trial 有效性/匹配关系 | 保留 |
| `app_cmd_net_async_cleanup` | operation cleanup 回调，收尾 job、trial、netcheck/Wi-Fi 恢复 | 核心保护逻辑，禁止为拆文件而改写 |
| `app_cmd_net_wifi_error` | 将 Wi-Fi 试连结果映射为 operation 错误 | 保留 |
| `app_cmd_net_async_run` | worker 执行试连、联网验证、阶段更新和 operation 状态迁移 | 核心逻辑，整体迁移 |
| `app_cmd_net_tryconnect_async` | 参数校验、幂等/资源受理、创建 operation | 保留 |
| `app_cmd_net_apply_async` | 校验 trial 和 owner，进入提交阶段并执行 apply | 保留 |
| `app_cmd_net_cancel_async` | 校验取消权限并取消 operation | 保留 |

以上函数应作为一个逻辑单元迁入 `cmd_net_txn.c`。迁移时不应把 `s_net_async`、`s_net_trial`、锁和阶段快照改为跨文件可写的 extern 全局变量。

### 2.2 状态响应构造

| 函数 | 职责 | 处理建议 |
|---|---|---|
| `app_cmd_build_network_status` | 组装 Wi-Fi、MQTT、profile、trial 和联网验证状态 | 迁入 `cmd_status.c`，改为先取快照再构造 JSON |
| `app_cmd_reset_reason_name` | reset reason 枚举转字符串 | 可留状态模块 |
| `app_cmd_ota_state_name` | OTA image state 转字符串 | 可留状态模块 |
| `app_cmd_build_gpio_status` | 组装 GPIO 状态与引脚信息 | 迁入状态模块 |
| `app_cmd_build_uart_runtime` | 组装 UART 参数和运行统计 | 迁入状态模块 |
| `app_cmd_build_device_status` | 汇总固件、heap、BLE、MQTT、分区和网络状态 | 迁入状态模块 |
| `app_cmd_build_device_info` | 组建设备静态信息 | 迁入状态模块 |
| `app_cmd_build_get_config` | 按 domain 生成配置读取响应 | 与配置命令模块边界一起评估 |

状态构造函数应只负责快照读取和 JSON 组装，不应持有网络事务锁完成较长时间的 JSON 分配。

### 2.3 配置与通用辅助函数

| 函数 | 职责 | 处理建议 |
|---|---|---|
| `app_cmd_patch_allowed` | 按命令来源限制配置 patch 域 | 保留，迁入配置命令模块 |
| `app_cmd_verify_web_password` | 校验管理口令 | 保留安全语义；后续评估去掉 HTTP 专属命名 |
| `app_cmd_parse_bool_param` | 解析兼容布尔参数 | 保留 |
| `app_cmd_domain_known` | 校验配置 domain | 保留，评估与 config 模块统一定义 |
| `app_cmd_domain_wanted` | 判断请求是否选择指定 domain | 保留 |
| `app_cmd_add_txn_fields` | 添加配置事务/effect 响应字段 | 保留 |
| `app_cmd_run_patch` | 执行配置 patch 并转换错误 | 保留，迁入配置命令模块 |
| `app_cmd_save_profile_from_params` | 按配置变更情况保存当前 Wi-Fi profile | 保留但需澄清命名和调用前提 |
| `app_cmd_m1hook` | 故障注入测试入口 | 仅测试构建保留 |

### 2.4 统一命令分发入口

`app_cmd_execute_method()` 是主要入口，约 886 行。它当前负责：

1. 初始化输出、校验 command context 和 method；
2. 统一执行 network policy 检查；
3. 分发 config、device、GPIO、UART、data、network、operation 命令；
4. 解析每条命令的参数、来源权限和业务条件；
5. 调用业务模块；
6. 统一处理 JSON 所有权、错误和 `app_cmd_result_t`。

它应继续作为统一入口，但逐步只保留 context 校验、统一策略、命令分发和统一收尾。具体命令逻辑应由小型 handler 承担。

## 3. 主要调用关系

### 3.1 传输入口到统一命令层

```text
BLE CMD / ThingsBoard MQTT / 内部调用
        |
        v
app_cmd_context_t（来源、身份、权限等上下文）
        |
        v
app_cmd_execute_method()
        |
        +-- config.get / config.set
        +-- device.info / device.status / network.status
        +-- io.get / io.set / relay.* / led.*
        +-- uart.send / uart.runtime / data.loopback
        +-- network.scan / profile.* / reconnect
        +-- network.tryconnect / apply / cancel / operation.get
        |
        v
config / wifi / operation / GPIO / UART / data_pipe / BLE / MQTT 等模块
```

### 3.2 网络异步事务

```text
network.tryconnect
  -> app_cmd_net_tryconnect_async
  -> app_operation_start
  -> app_cmd_net_async_run (worker)
  -> app_wifi_try_connect_ip
  -> netcheck / MQTT 验证
  -> 保存 trial 与阶段快照
  -> operation 状态更新（如 GOT_IP）

network.apply
  -> app_cmd_net_apply_async
  -> 校验 operation、owner、trial
  -> COMMITTING
  -> 现有内部同步 apply 路径
  -> 配置提交 / profile 保存 / restart 调度
  -> cleanup 与终态

network.cancel
  -> app_cmd_net_cancel_async
  -> app_operation_cancel
  -> cleanup

operation.get
  -> operation 状态与 owner 校验
  -> trial / 阶段快照 / live 状态响应
```

当前 apply 路径通过内部 context 再入统一命令入口，复用同步 apply 逻辑。该复用意图应保留；后续可把同步提交主体抽为私有函数，但第一步不要复制第二套 apply 算法或改变再入时序。

## 4. 锁与并发审查

### 4.1 已识别的锁

| 锁/guard | 保护对象 | 结论 |
|---|---|---|
| 网络 job 递归互斥锁（`s_net_job_lock`） | 网络异步 job、trial 及关联字段 | 保留递归属性；不可直接替换普通 mutex |
| 阶段快照临界区（`s_stage_snap_mux`） | operation 阶段快照数组 | 保留；临界区内只做短小内存操作 |
| scan gate 临界区（`s_scan_gate_mux`） | 扫描节流时间戳 | 保留 |
| `app_config_lock()` | `s_config` 配置结构 | 继续由配置模块管理 |
| `app_wifi_runtime_lock()` | Wi-Fi 运行态 | 继续由 Wi-Fi 模块管理 |
| `app_mqtt_lock()` | MQTT 运行态 | 继续由 MQTT 模块管理 |
| `app_cmd_cfg_txn_guard_enter/exit()` | 配置事务占用 | 保留，不能与 job lock 合并 |
| Wi-Fi profile 内部锁 | profile/NVS 数据 | 独立于 config runtime lock，锁序需全局核对 |

### 4.2 明确整改项

- **`io.get` 读取 Wi-Fi 运行态未见 runtime lock 保护**：读取 `s_wifi.sta_has_ip` 和 `s_wifi.sta_ip` 应在 `app_wifi_runtime_lock/unlock` 内复制快照，锁外生成 JSON。
- **状态 JSON 构造期间持有锁**：`app_cmd_build_network_status` 在 Wi-Fi/job 锁内执行多次 cJSON 添加；应改为锁内复制数据，锁外构建 JSON。
- **job 字段读取边界不统一**：`s_net_async.active`、验证状态和上下文在多个入口/worker/cleanup 路径被访问。应盘点每个读写点，统一提供加锁快照接口；不能仅凭当前静态阅读把每个锁外读取都定性为已复现竞态。
- **配置锁与 profile 锁序需核对**：`app_cmd_save_profile_from_params` 路径可能在持有 config lock 时进入 profile 保存/NVS 路径。需与 Wi-Fi 事件回写 profile 的调用链一起检查，避免锁序反转。
- **operation/job 锁契约需验证**：确认 operation 内部临界区不回调 cmd 层，run/cleanup/cancel 回调均在 operation 锁外执行；如该契约成立，应写入接口注释并作为拆分约束。

### 4.3 不应破坏的保护

1. 网络 job 锁递归语义及其保护范围；
2. operation ID/generation 复核，避免旧 worker 操作复用后的槽位；
3. cleanup 的幂等性和资源释放路径；
4. tryconnect/apply/cancel/expire/BLE disconnect 的竞争处理；
5. 配置事务 guard；
6. 阶段快照与终态查询语义；
7. scan gate 的短临界区保护。

## 5. 重复逻辑与合并方案

### 5.1 GPIO 控制分支

`led.toggle`、`relay.toggle`、`led.set`、`relay.set`、`io.set` 重复执行“读取前态—确定目标值—调用 `app_io_set_outputs`—读取后态—生成 changed 响应”。

建议抽出一个内部 IO 执行 helper，统一执行与 changed 计算；保留五个协议命令名、参数兼容和响应语义，不将旧命令直接删除或强制迁移为单一 `io.set`。

### 5.2 apply 路径

异步 apply 与同步 apply 有重复业务意图，但异步层还负责 operation 状态、owner 和 cleanup。保留当前复用关系；后续抽取同步提交主体，异步层负责生命周期，配置层负责提交。不要把网络 rollback/restart 事务泛化成普通 config patch。

### 5.3 状态响应

`device.status` 复用 `network.status` 构造是合理的，应保留。建议统一各模块快照采集接口，避免不同命令重复读取同一组运行态。

### 5.4 profile 保存

`app_cmd_save_profile_from_params` 的命名容易误导：它根据请求是否涉及 SSID 变更触发保存，但读取的是当前配置结构。建议改成能表达“保存当前有效配置”的名称，并明确调用前提；确认调用者后再改名。

## 6. 保留、合并、删除清单

| 项目 | 决策 | 说明 |
|---|---|---|
| 网络 async run/cleanup/trial/operation | 保留 | 核心事务保护，不为减行数重写 |
| job 锁、generation、阶段快照、scan gate | 保留 | 并发和状态一致性保护 |
| GPIO 五类命令 | 合并内部执行 helper | 保留外部命令和兼容语义 |
| 状态 JSON 构造 | 抽离并统一快照采集 | 不改变字段协议 |
| config patch 与错误映射 | 保留并归入 config handler | 统一配置语义 |
| apply 同步提交主体 | 后续抽取 | 保持现有异步/同步复用 |
| adapter/wrapper | 暂留 | 先查全仓调用方，保持接口兼容 |
| `diag.m1hook` / `diag.testhook` | 仅测试构建 | 确认生产构建不包含测试入口 |
| HTTP 专属命名/来源分支 | 待全仓确认后处理 | 不凭局部文件直接删除枚举、owner 或兼容路径 |
| 未引用的内部 wrapper | 待全仓调用检索后删除 | 必须证明无调用者 |

## 7. 推荐目标拆分

```text
cmd.c             context 校验、统一 policy、分发、响应收尾
cmd_net_txn.c     tryconnect/apply/cancel/operation、job/trial 生命周期
cmd_net.c         scan/status/profile/reconnect 等普通网络命令
cmd_config.c      config.get/set、domain、patch、配置权限
cmd_device.c      GPIO、IO、UART、data.loopback
cmd_status.c      device/network/gpio/uart 状态与 info JSON
cmd_internal.h    最少量内部类型/函数声明（避免暴露事务全局变量）
```

约束：
- 不引入新框架、事件总线或复杂动态对象体系；
- `s_net_async`、`s_net_trial`、锁和阶段快照留在 `cmd_net_txn.c` 私有；
- 新模块通过窄函数接口访问事务能力，不使用 extern 暴露可写全局状态；
- 对外 command context、命令名称、JSON 字段和错误码保持兼容；
- 拆分与行为变更分开提交。

## 8. 审查限制与下一步

本轮是静态审查。以下内容仍需通过后续模块审查确认：
- operation 的锁、回调、取消、过期和槽位复用语义；
- 配置事务 guard 与 job lock 的组合；
- profile lock 与 config lock 的全局锁序；
- 全仓 adapter/HTTP 来源/测试 hook 调用者；
- 所有命令的旧客户端兼容和错误码。

下一轮建议审查 `operation.c`、配置事务 guard 所在模块及其调用方，再确定 `cmd_net_txn.c` 的接口边界。代码迁移前先建立回归矩阵，至少覆盖 tryconnect/apply/cancel/expire、BLE 断连、联网验证失败、重复请求、配置保存失败和 cleanup 失败。
