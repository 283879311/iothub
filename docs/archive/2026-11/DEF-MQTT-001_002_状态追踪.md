# DEF-MQTT-001 / DEF-MQTT-002 状态追踪（唯一活文档）

> 本档合并了立项建议、修复方案评审、r1/r2 实现核验四份过程文档的结论；原始过程稿在 `docs/archive/2026-11/`。**当前状态更新只改这一份。**

## 状态一览（2026-11-05 23:0x，001/002/003 全部闭环，本档转归档）

| 项 | 状态 | 下一步 |
|---|---|---|
| 001 主修复（回包先出站再拆 WiFi） | **✅ 闭环**（V6/V2' r2b 通过） | 无 |
| 002 短期（独立过期任务 op_expire） | **✅ 闭环**（E1/E3 FI 构建真机验收通过，2026-11-05 21:0x） | 无 |
| 002 长期（run_fn 非阻塞化） | 未开工 | 独立评审，不急（归档提醒：此项未入排期清单） |
| **003**（cleanup 不恢复持久配置） | **✅ 闭环**（B 方案，R1–R4 全 PASS，2026-11-05 22:0x，BIN `50c75bc4…`） | 无 |

> **003 验收终态（2026-11-05 22:06）**：R4 41 例 39/0/2；R2 全串口 rescue=0（apply 接管与失败路径零误伤）；R3 cancel 形态 `rescue → apply_begin mode=static → .50`；R1 expire 形态 got_ip+~289s 准点自愈。报告 `docs/DEF-MQTT-003_设备验收报告_2026-11-05.md`（`dab5d859…`）。残余记录：run_fn 失败路径恢复——**11-05 晚审计关闭（前提不成立，见 `docs/待排期清单_2026-11-05.md`）**；订阅态僵尸不恢复与 BLE-001 对价同类，见待排期清单 P3。

## 1. 缺陷定义

- **001**：MQTT 源 `network.tryconnect` 的受理回包（QoS1）未及 ACK，**试验自己的 WiFi 断开**（mqtt.c:1877 `STA_DISCONNECTED → app_mqtt_stop()`，非 MQTT rebuild）就拆了 TCP → 回包丢失 → 发起端不知道 operation_id → 孤儿 trial 占操作槽（后续 busy）。BLE 源无此竞态。
- **002**：operation worker 单任务同步跑 run_fn（`op->running=true` 到返回），run_fn 阻塞期间 1s 节拍停摆 → `expire_one` 饿死 → ACCEPTED 20s TTL 形同虚设（现场孤儿撑到 195s 无 expire）。**r1 验收（V2/V3 FAIL）实证**。

## 2. 关键结论（评审定案，勿回退）

1. **等待禁止放在 esp-mqtt 事件任务**——PUBACK 事件在同一个任务里派发，同步等待必然超时。所有等待都在 worker / 重启任务侧。
2. **真拆链点是试验 WiFi 断开**，不是 MQTT rebuild——闸门必须前移到 tryconnect run_fn 第一次 WiFi 动作之前；MQTT 重启点的闸门保留为第二道。
3. **TTL 不改、不加新错误码**——ACCEPTED TTL 已是 20s，过期路径已调 `defer_restart_end`（operation.c:669）；002 修好后自愈即成立。`timeout` 复用现有错误映射。
4. **E1/E2 用 `APP_WIFI_FAULT_INJECT` connect-hold 造孤儿**——真实 AP 造不出 ACCEPTED>20s（能连秒到 got_ip，连不上 15s 转 failed），口令法已证伪。
5. **V7 结案**：+4.95s 提前拆链 = 试验 WiFi 断连，非 defer 提前放行（唯一 defer_end 在 +15.2s）。

## 3. r2 两个口子 → r2b 已修复并复核通过（2026-11-05 19:57）

1. **publish 先置 PUBLISHED 位后武装** → **已修**（mqtt.c:826-840）：publish → `arm(msg_id)`（锁内写 pending + 起 1s 定时器）→ `ack_gate_setup()` → 置 PUBLISHED 位。事件组操作自带内存屏障，worker 见位必见 pending。✅
2. **begin 只清 PUBLISHED 不清 ACK_GATE** → **已修**（mqtt.c:765-772）：`resp_delivery_begin()` 同时清两位。begin 时尚未 publish，任何已置位均属上一笔陈旧信号。✅

复核补充确认的三个次级语义（均无问题）：

- `ack_gate_setup()` 只建组/建定时器 + 清 **ACK_GATE_BIT**（不清 PUBLISHED），位于 arm→setbits 之间不误伤本次武装
- `on_published` 匹配即清 `s_pending_resp_msg_id` + 停定时器 → wait 后 `pending==0 → "acked" / 非 0 → "timeout"` 日志判据成立（V2' 采证以此为准）
- publish 早于 begin 的反转时序（worker 起晚）：① 白等 300ms，② 仍由 armed pending 保护——若 PUBACK 已在 begin 前消费（pending=0），② 直通但回包实际已 ACK，无风险

**r2b 构建**：BIN `eade947018a3…99c5b5`（1,160,864 B）、ELF `8aed4529…`、0 warning；cmd_policy 50 / cmd_cfg_txn 22 / cmd_error 全绿；工件 `artifacts/mqtt_ack_gate_r2b/`。mqtt.c SHA `fbbe79ea…`。

## 4. 验收矩阵

| 用例 | 内容 | 状态 |
|---|---|---|
| V1 | 正常 41 例回归 | ✅ r1 已过（39/41，2 SKIP 预期） |
| V2' | cut 注入：worker 侧 `armed→(acked\|timeout)→released` 全部在 `tryconnect ssid=` 之前 | ⬜ 待 r2 |
| V3' | 孤儿自愈：`expire cleanup=ok` + run_fn `stale after wait` 同时出现（R5 防线证据） | ⬜ 待 r2（FAULT_INJECT 构建） |
| E1 | **执行卡已核验，批准烧写 FI `884d4e6c…`**（2026-11-05 20:52）：旋钮实现四点确认（整体 FI 门控、clamp [500,60000]→invalid_param、take 即回落 one-shot、#else 保留 20000/300000 字面量，GOT_IP_TTL 300000 两分支均为字面量）；工件 SHA 与卡一致；生产 SHA 漂移实证=恰 64B（offset 177–240 descriptor ELF-SHA + 尾部 32B 校验区，其余逐字节同）。判据 E1-a/b/c/d + 反例四条均与 hold/cancel 语义吻合 | ⬜ 待烧执行 |
| E2 | **重定界：原表述结构性不可测**（单活动槽，`app_operation_start` 遇活动 op 返 INVALID_STATE——这正是 busy 的来源）。解耦价值已由 E1 覆盖（expire 在 run_fn 阻塞期间触发即证明）；补充观测项：expire 后槽立即可复用（新 create 成功）→ 已并入执行卡 E1-d | ✅ 并入 E1 |
| E3 | FI+C1 合并构建 `1721fe1c…`（5 条 warning 为 bt.c/uart.c 的 C1 组合 pre-existing，非本次改动）：`expire_post_cleanup` pause + 并发 cancel，判据=单 cleanup、唯一终态 | ⬜ 待烧执行 |
| V6 | 41 例 + N06 回归不回退 | ✅ r2b（39/41，2 SKIP；闸门新时序 `resp→armed→acked` 60ms 先于 `tryconnect`） |
| ~~V7~~ | defer_end site 埋点定性 +4.95s | ✅ 已结案（§2.5） |

### r2b 验收补充记录（2026-11-05 20:15）

- **V2' 判据成立**：`armed(202117) → released(203207) → tryconnect(208127)`，回包窗口关闭。`released msg_id=4242 (timeout)` 为注入脚本重发 3 次触发限频覆盖 pending 所致（脚本已改单次），语义无影响。
- **V3' 部分达成**：真实 AP 造不出 ACCEPTED>20s（2.6s got_ip → GOT_IP TTL 300s），但**解耦任务实证**：`expire cleanup=ok` + `defer_end site=expire/terminal` 在 got_ip+300s 准点触发，槽释放。
- **遗留 = DEF-MQTT-003**（§6）：cleanup 后设备停在试验网络 .28（DHCP），`cfg_state=static`/`persistent_static_ip=.50` 未恢复，手动 reconnect 才回。
- **安全加固**：注入 broker 加 `--expect-client`（默认 C8E6 client_id 串），不匹配断链；每连接单次请求。外部设备 `192.168.1.4` 曾连入但未收到任何请求、未改状态。

### E1/E3 真机验收结果（2026-11-05 21:0x，双 PASS）

- **E1 PASS**：`accepted(44996, ttl=3000) → expire cleanup=ok(48986, t+3.99s) → stale after wait(49996, gen=1 state=8)`——expire 比 run_fn 返回**早 1.0s**，解耦实证；槽复用 E1-d PASS（二次 tryconnect `accepted` + cancel 收尾）。
- **E1-b 判据修正**（卡面假设被实测推翻，修正成立）：原表述"cleanup 打不断 hold、run_fn 跑到 10s"基于"hold 必被试验领取"的假设——**实测 tryconnect 试验路径不走 `connect_begin`**（领取 path 仅 profile/fast_path/pmf_retry/retry_direct，wifi.c:919/1015/1500/2094），hold 在试验期间无人领取，run_fn 是 expire 后 1s 因 `try_cancel` 返回、hold 由随后的 fast_path 自动重连领取（50396）。修正后判据：**expire 必须早于 run_fn 返回**——本轮满足且余量 1.0s。
- **E3 PASS**：`expiring` 闸拒绝并发 set_state 探针（`ESP_ERR_INVALID_STATE`）与并发 cancel（`operation_state`），pause 2.2s 内无二次 cleanup，终态唯一（expired/timeout）。R3/R4/R5 并发防线首次真机实证。
- **测试钩子 v1 白名单**：`diag.m1hook`（FI）/`diag.testhook`（C1）进 v1 注册表，各自 `#ifdef` 包裹、均 SRC_BLE only——生产镜像不含，已核验（cmd_v1.c:52-60）。
- **生产镜像基线更替**：回烧 `f26caef5…`；与已验收 `eade9470…` 实测恰差 65B（同两处派生哈希区，cmp 实证），可执行内容等价。**后续烧写/取证一律写 `f26caef5…`**。
- 证据：`docs/E1_E3_验收报告_2026-11-05.md`（`ace5c886…`）+ `artifacts/e1_e3/`（串口全文 serial_e1/e1b/e3.log、amqtt 日志、SHA256SUMS）。

## 6. DEF-MQTT-003：孤儿清理不恢复持久配置（已批开工，2026-11-05）

**现象**：trial 到 got_ip（试验网络）后 TTL 到期，`expire cleanup=ok`、槽释放，但运行态停留在试验网络（.28 DHCP）；持久配置 static .50 不生效，需手动 `network.reconnect` 恢复。

**代码实锤**（我方复核）：`app_cmd_net_async_cleanup`（cmd.c:287-305）仅做 ① CONNECTING 态 `app_wifi_try_cancel()` ② 锁内清 trial/job（R2-P0），**无任何 WiFi 恢复动作**；`expire_one`（operation.c:617-678）转终态 + defer_end 后也不触发重启。cleanup 的"成功"只指簿记清理，与网络形态无关。

**判别口径裁定（2026-11-05 21:32，选 B）**：

1. **事实修正（缺口比表述的窄）**：`app_operation_cancel` 在终态化**之前**调用 cleanup（operation.c:458-522，`cancel_post_cleanup` pause 钩子即证）；断连处理里 op 处于 active（ACCEPTED/CONNECTING/GOT_IP）时走 `can_cancel → cancel → cleanup`，trial 由 cleanup 亲手清——**A 在这条路径上同样会恢复**。fall-through 直清 trial 仅在 op NULL/终态时可达（cmd.c:263-283：can_cancel→cancel→return；COMMITTING→return）。故"断连清 trial 后 op 才过期"这一形态在现状态机下基本不可达。
2. **仍选 B 的理由**：语义正确——"这次清理有没有提交新配置"是不变量，"trial 结构体在不在"是簿记表达力；且实测等价性成立：expire 选中的 op 永远非 COMMITTING（set_state(COMMITTING) 置 deadline=0，expire 跳过 deadline==0），cancel 拒绝 COMMITTING → **B 在全部可达路径上 ≡ A 的超集，且零额外代价**（用户担心的"CONNECTING cancel 多一次重启"不成立：A 下 trial 同样 valid 被 cleanup 清掉，同样恢复）。
3. **B 的两个实现条件**：① state 判别读取与 trial 检查同一次 job lock 临界区（单点决策，可审计）；② 恢复调用在锁外（`app_wifi_schedule_restart` 异步，restart 任务执行时才 `build_cfg_from_config` 读最新持久配置——即使与并发 apply 竞态也收敛到最新提交，竞态安全已核）。
4. **R2 口径保持严格**：apply 失败路径（COMMITTING→FAILED）不经过 cleanup（cleanup 仅 TTL/cancel），同样不得出现 rescue 日志。
5. **残余缺口记录（A/B 共同，本批不修）**：run_fn 失败路径不调 cleanup（operation.c:598-613 直接置 FAILED，R4 expiring 分支除外）——got_ip 后 run_fn 失败的场景设备留试验网络，cleanup 永不运行，A/B 均不覆盖；BLE 断连 fall-through 直清同理。需"cleanup 之外"的机制（run_fn 失败尾部恢复），建议单独立项观察，不混入本批。

**验收判据（建议）**：R1 孤儿 got_ip 后 ≤25s（TTL 20s 内不适用 GOT_IP 300s 场景时以实际 TTL 为准）运行态回持久配置（.50/static）+ `cfg_state=static`；R2 apply 已接管场景不触发恢复；R3 恢复不与 apply 竞态（job_lock 判别）；R4 41 例回归不回退。

**与 002 的关系**：同属孤儿清理逻辑（002 修"槽释放"，003 修"网络形态恢复"），但修复点不同（cmd.c cleanup vs operation.c expiry），单独一批，不混烧。

## 5. 档案索引

- 过程稿：`docs/archive/2026-11/`（立项建议 / 修复方案评审 / r1 实现核验 / 静态 re-apply 系列 / 阶段 0 系列）
- 现行决策：`docs/DEF-MQTT-001调整与002立项决策_2026-11-05.md`、`docs/ackgate_r2核验_2026-11-05.md`
- 硬件侧证据：`iothub/docs/MQTT受理回包竞态_复现与代码路径_2026-11-05.md`（SHA `bdc38f38…`）、实现记录（`14aee933…`→`49690eed…`）、验收报告（`0bfa7021…`）、`artifacts/mqtt_ack_gate*/SHA256SUMS`
