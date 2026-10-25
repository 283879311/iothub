# T5 首轮 50 轮 soak（runId t5-224658306-BD75）

- 时间 23:03:03 · 用时 965s · 50 轮 mixed · serial · 目标=正式=CMCC-502
- 测试器版本：修复前（本报告的 C 轮失败取证缺失，是已知缺口，v2 已补）
- **解读**：
  - 槽语义实证：全程 ~25 条终态记录 / 16 分钟，no_slot=0、busy=0、悬挂=0。旧固件（600s 保留、无覆盖）在第 5-8 轮就会 no_slot，本 run 一次未出 → 分级保留 + 槽满覆盖**已在设备上生效**（串口 `reclaim oldest terminal` 日志待对时确认）。**勘误 2026-10-25**：本 run 实际证明的是**覆盖淘汰**生效；分级保留经评审回退（统一 600s），该点从结论中移除。
  - A/B/D 三类全部符合设计：A 15/15 PASS、B 15/15 EXPECTED（ap_not_found 与 wrong_password 独立分类，无污染）、D 9/10 PASS。
  - **C（apply）10/10 FAIL**：设备受理 tryconnect 后 ~1s 内 op 直达 failed(connect_failed)。每对 C 都紧跟 B3（ap_not_found）之后；B1（错口令到同一 AP）能完成 16-20s 的完整 4 次握手，说明射频/关联正常，失败不是口令或信号问题。当时测试器丢失 operation_id、且未记录口令是否存在 → 无法与固件对账。v2 测试器已补：失败轮自动找回 op id 并抓 operation.get + network.status（last_disconnect）证据；新增「配网场景」单集可只跑 C 轮复验。
  - **主机侧复现结论（2026-10-25 补充，`tools/ble_p0/t5c_anomaly_repro.py`，结果 `t5c_repro_20261025_114139.json`/`114340.json`）**：
    - **B3→C 固件串扰假设被排除**：B3(ap_not_found) 后 ~1s 用正确口令 tryconnect → 正常 `got_ip`（两轮 14.8s/16.3s）。
    - **发现真实的固件缺陷（P1-10，与 T5 C 不同因）**：候选拆除后的重连扰动窗口（got_ip→cancel 后 ~5s 内）里，tryconnect 对**不存在的 SSID** 也返回 `connect_failed@~3s`（2/2 复现：3.05s/3.3s），干净态同操作是 `ap_not_found@7.65-7.85s`。机理（源码）：`try_connect_common` 预扫描失败时**跳过在场检查直接连接**（fail-open），该窗口内 STA 断开/驱动过渡态使扫描失败或连接即败。错口令在该窗口同样偏移为 `connect_failed@5.1s`（干净态 `wrong_password@13.5-14.8s`）。
    - **T5 C 最可能根因指向测试器口令为空**：空口令实测 `failed/connect_failed`（last_disconnect=36(OTHER)）——分类与 T5 C 一致；"10/10 全部同形态失败"符合确定性成因（口令缺失），不符合扰动窗口的随机命中（soak 平均轮距 ~18s，10 轮全落 ~5s 窗口概率极低）。次级可能：扰动窗口机理（时间形态吻合）。**v2 测试器复验一轮即可分辨**：记录口令是否为空 + last_disconnect reason；若口令非空且稳定复现，才升级为固件问题并按 P1-10 修复方向处理。
    - 附带确认：错误分类有时变性（wrong/empty password 不必然报 wrong_password，可报 connect_failed）——小程序文案对 connect_failed 已按"连接失败可重试"处理，无需改。
  - 观察项：A 轮 got_ip 恒定 ~42s（#13 达 82s），疑似全信道扫描耗时；D 轮奇数轮查询时设备尚未回网（mqtt=0/ip=无），偶数轮（+4s）已恢复 → 管理态恢复 2.4-3s，网络恢复 4-7s，作为 M3 三分计时基线。
  - 测试器渲染 bug（已修）：`环境: [object Object]`、D 轮 `ip=undefined`。

## 原始报告

T5 常态化运行与异常韧性验收报告
runId: t5-224658306-BD75
时间: 23:03:03.693 · 用时 965s · 设备: IotHubBLE-C8E4 (B0:A7:32:28:C8:E6)
环境: [object Object]
配置: 50 轮 · mixed · serial · 目标 SSID=CMCC-502 · 正式 SSID=CMCC-502

汇总: 共 50/50 轮 · PASS 24 · EXPECTED 15 · FAIL 11 · no_slot=0 · busy=0 · 悬挂操作=0 · 重补链=10 次
耗时: min 852ms / avg 18124ms / max 82244ms
  tryconnect-cancel: 15 轮 (pass 15 / expected 0 / fail 0)
  designed-failure: 15 轮 (pass 0 / expected 15 / fail 0)
  apply: 10 轮 (pass 0 / expected 0 / fail 10)
  relink: 10 轮 (pass 9 / expected 0 / fail 1)
错误分布: ap_not_found×10 · wrong_password×5 · connect_failed×10 · relink-failed:scan×1

逐轮要点（完整逐轮记录见会话原文；此处存目录级证据）:
- A 轮 #1/#2/#3/#11/#12/#13/#21-23/#31-33/#41-43: connecting → got_ip → cancel-sent, 终态=cancelled, 耗时 41644-82244ms
- B 轮 15 轮: ap_not_found×10 (6149-10348ms) / wrong_password×5 (16398-19949ms), 终态=failed, config.get 核对正式 SSID 未被覆盖
- C 轮 #7/8/17/18/27/28/37/38/47/48: tryconnect+apply, tryconnect 受理后 852-3647ms 直达 failed(connect_failed), op id 未记录（测试器缺口）
- D 轮 10 轮: 奇数轮查询时 ip=无 mqtt=0, 偶数轮 ip mqtt=1 conn=CMCC-502, link 2042-2956ms; #9 relink-failed:scan (9080ms)
- 备注行: 堆曲线以串口 diag 对时；协议层 heap 字段属 T5 二期候选

汇总行与错误分布为原始输出；逐轮记录以会话粘贴原文为准（含每轮原始请求与脱敏口令标记）。
