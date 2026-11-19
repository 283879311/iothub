# T4-A 设备侧前置证据（2026-11-02）

> 结论：以下只是**设备侧/BLE central 代理**证据，不是 T4-A PASS。Android 微信/iOS 端的 #15/#17/#22–#24 仍需真机执行；iOS 无设备，T4-iOS 继续 BLOCKED。

环境：`test/c1-mainline` @ `a35eef9`，A `IotHubBLE-C8E4` / `B0:A7:32:28:C8:E6`。

## #17 token 大小写

- 脚本：`tools/ble_p0/a_token_board_test.py --address B0:A7:32:28:C8:E6`
- 结果：`tools/ble_p0/results/a_token_board_20261102_081358.json`
- **24/24 PASS**，`password_restored=true`：
  - 大写 token 有效；
  - 小写 token v1 返回 `unauthorized`；
  - 小写 token v0 返回 `auth_required`；
  - 改密/恢复后旧 token 失效、原口令可登录。
- 边界：这是设备侧 token 契约；Android 微信 `tokencase` 仍需按 T4-A 执行，不能替代 App 端。

## #22 固定 NUS 128-bit Service UUID

- 扫描证据：`tools/ble_p0/results/t4a_22_24_scan_20261102.txt`
- BlueZ/Bleak 以 `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` 过滤扫描，A/B 均命中；两台 `service_uuids` 都含该 UUID。
- 设备侧结论：**PASS**，固定 UUID 已进入广播，按 services 过滤可发现。
- 边界：Android 微信 `services` 过滤的实际行为仍需真机执行（#22 App 端）。

## #24 ADV 短名 / Scan Response 完整名

- 源码：`main/bt.c:338-345`（`IH-` + 完整名后 4 位），`main/bt.c:357-383`（ADV=flags+UUID128+短名；Scan Response=完整名+tx power）。
- 扫描合并结果：A 显示 `IotHubBLE-C8E4`，B 显示 `IotHubBLE-65D8`；Bleak/BlueZ 合并 ADV+Scan Response 后只暴露完整名，短名未单独暴露。
- 设备侧结论：**配置/合并扫描 PASS**；原始 ADV/Scan Response 分工由源码确认。`btmon` 因无 HCI 绑定权限（`Operation not permitted`）未能抓原始 AD，不能声称做过空口逐字段拆分。
- 边界：iOS/Android 微信实际名称显示待测（#24 App 端；iOS BLOCKED）。

## #23 CMD TX 分片与长 JSON 重组

- 证据：`tools/ble_p0/results/t4a_23_tx_timing_20261102.json`
- 设备返回 `config.get` **1130B**；MTU256 下 5 个通知（253×4 + 118），JSON 完整解析，无截断。
- 设备源码：`main/bt_gatts_nimble.c:428-445` 每片后 `vTaskDelay(20ms)`；`main/ble_cmd.c:44-...` 使用独立 `ble_cmd_tx` 队列，命令任务不阻塞。
- 实测通知间隔（首片后）：44.02 / 88.70 / 46.81ms，中位数 46.81ms；不是严格的 20ms。原因是设备连接间隔约 45ms，通知只能落在连接事件上；20ms 是设备侧最小延时，不是空口保证。**其中 88.70ms 超出 20–50ms 设计区间**，不能把 #23 的间隔门禁写成已通过。
- 设备侧结论：**长响应完整、分片路径工作；间隔门禁未签**。App 端 Android/iOS 连续长 JSON 重组仍待测（#23 App 端）；需在真机复测并判断 88.7ms 是连接事件调度还是通知丢失/重排。

## 未覆盖/不能替代

- #22–#24 的 App 端：Android 微信真机；iOS 无设备 BLOCKED。
- #13 DATA 断连、#14 无 AP 救援、#25：现有记录不足以还原验收条件，**不猜**，待验收中心给出明确步骤后再执行。
- #15 Android 权限/MTU：待手机手工记录；PC 侧 MTU 256 结果不能替代微信 `wx.setBLEMTU` 行为。
- 角色互换：HTTP 入口已删除，N/A（原 soak_test_plan 过程稿已不在当前仓库；HTTP 入口移除后的 N/A 结论仅属当时测试环境）。
