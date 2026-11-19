# BLE P0 历史过程材料索引

> 文档状态：HISTORICAL  
> 本索引管理 `tools/ble_p0/results/` 下的 45 份 Markdown 过程/说明文档。目录内原始 JSON、日志、补丁、哈希和二进制证据共存；本索引不改写、不替代原始证据。

## 使用边界

- 本目录是历史取证与过程材料，不是当前固件 1.3 的验收入口，也不是当前设备操作计划。索引列出的 45 份 Markdown 均继承本索引的 `HISTORICAL` 状态；单份记录中的 PASS/FAIL 只描述原测试轮次。
- `*_RESULT.md`、`REPORT*.md` 保存对应历史执行结果；`*_CARD.md`、`*_PLAN.md`、`*_DESIGN.md`、`*_APPLICATION.md` 是历史方案/执行卡/申请，不构成新的操作授权。
- 同目录下的 JSON、串口日志、patch、NVS/分区镜像和 SHA 文件是原始材料，保留原路径以维持证据引用。
- 文档中出现的 `build/iothub.bin`、`build/iothub.elf` 是当时本地构建生成物，不是仓库跟踪文件；需按对应构建基线重新生成，不应作为仓库链接检查的缺失文件。
- 当前 BLE 功能、未完成事项和重新验收门槛以 [BLE 架构与验收总览](../../../docs/architecture/BLE_架构与验收总览.md) 为准；重新执行必须重新评审固件 SHA、设备身份、测试范围和授权。

## 文档目录（45 份）

### CMCC-502 tryconnect 补丁与评审

- [README.md](cmcc502_tryconnect/README.md)
- [MERGE_REVIEW.md](cmcc502_tryconnect/patch_20261102/MERGE_REVIEW.md)
- [PATCH.md](cmcc502_tryconnect/patch_20261102/PATCH.md)
- [PR_BODY.md](cmcc502_tryconnect/patch_20261102/pr_split/PR_BODY.md)
- [PR_DESCRIPTION.md](cmcc502_tryconnect/patch_20261102/pr_split/PR_DESCRIPTION.md)
- [README.md](cmcc502_tryconnect/patch_20261102/pr_split/README.md)

### 双设备 soak 记录

- [REPORT.md](dual_soak_20261102/REPORT.md)
- [REPORT_clean_6h.md](dual_soak_20261102/REPORT_clean_6h.md)

### 静态 IP M1 / 启动恢复专项

- [G3A_FLASH_APPLICATION.md](static_ip_m1_20261103/G3a/G3A_FLASH_APPLICATION.md)
- [M1_FOLLOWUP_state_split.md](static_ip_m1_20261103/M1_FOLLOWUP_state_split.md)
- [T1_RESULT.md](static_ip_m1_20261103/T1_dhcp_boot/T1_RESULT.md)
- [T2_PRECHECK.md](static_ip_m1_20261103/T2_precheck/T2_PRECHECK.md)
- [T2_RESULT.md](static_ip_m1_20261103/T2_static_boot/T2_RESULT.md)
- [T3_PLAN.md](static_ip_m1_20261103/T3_PLAN.md)
- [T3_RESULT.md](static_ip_m1_20261103/T3_boot/T3_RESULT.md)
- [CRC_NOTES.md](static_ip_m1_20261103/T3_nvs_backup/CRC_NOTES.md)
- [T3_EXECUTION_CARD.md](static_ip_m1_20261103/T3_nvs_backup/T3_EXECUTION_CARD.md)
- [T3_STAGE_A.md](static_ip_m1_20261103/T3_nvs_backup/T3_STAGE_A.md)
- [T3_STAGE_A_ADDENDUM.md](static_ip_m1_20261103/T3_nvs_backup/T3_STAGE_A_ADDENDUM.md)
- [T3_STAGE_B0.md](static_ip_m1_20261103/T3_nvs_backup/T3_STAGE_B0.md)
- [T4_EXECUTION_CARD.md](static_ip_m1_20261103/T3_nvs_backup/T4_EXECUTION_CARD.md)
- [T4_T8_DEPENDENCY.md](static_ip_m1_20261103/T4_T8_DEPENDENCY.md)
- [T4_RESULT.md](static_ip_m1_20261103/T4_boot/T4_RESULT.md)
- [T5_T6_BOOT_INJECTION_DESIGN.md](static_ip_m1_20261103/T5_T6_BOOT_INJECTION_DESIGN.md)
- [T5_V1_RTC_PROBE_CARD.md](static_ip_m1_20261103/T5_V1_RTC_PROBE_CARD.md)
- [T5_V2_EN_RESET_CARD.md](static_ip_m1_20261103/T5_V2_EN_RESET_CARD.md)
- [T5_V2_EN_RESET_CARD_r2.md](static_ip_m1_20261103/T5_V2_EN_RESET_CARD_r2.md)
- [T5_V2_EN_RESET_CARD_r2.1.md](static_ip_m1_20261103/T5_V2_EN_RESET_CARD_r2.1.md)
- [T5_V2_EN_RESET_CARD_r2.2.md](static_ip_m1_20261103/T5_V2_EN_RESET_CARD_r2.2.md)
- [T5_V2_G2_FREEZE.md](static_ip_m1_20261103/T5_V2_G2_FREEZE.md)
- [GATE1_RESULT.md](static_ip_m1_20261103/T5_gate1/GATE1_RESULT.md)
- [T5_GATE1_HALT.md](static_ip_m1_20261103/T5_gate1/T5_GATE1_HALT.md)
- [T5_GATE1_NVS_METHOD.md](static_ip_m1_20261103/T5_gate1/T5_GATE1_NVS_METHOD.md)
- [T5_GATE2_EXECUTION_CARD.md](static_ip_m1_20261103/T5_gate1/T5_GATE2_EXECUTION_CARD.md)
- [STAGE1_RESULT.md](static_ip_m1_20261103/T5_gate2/STAGE1_RESULT.md)
- [STAGE2_RESULT.md](static_ip_m1_20261103/T5_gate2/STAGE2_RESULT.md)
- [STAGE3_RESULT.md](static_ip_m1_20261103/T5_gate2/STAGE3_RESULT.md)
- [STAGE4_RESULT.md](static_ip_m1_20261103/T5_gate2/STAGE4_RESULT.md)
- [V1_FINAL_SUMMARY.md](static_ip_m1_20261103/T5_gate2/V1_FINAL_SUMMARY.md)
- [D1_MQTT_SERVER_SIDE_CARD.md](static_ip_m1_20261103/stage0/D1_20261104/D1_MQTT_SERVER_SIDE_CARD.md)
- [D1_RESULT_PARTIAL.md](static_ip_m1_20261103/stage0/D1_20261104/D1_RESULT_PARTIAL.md)
- [V1_SELFTEST_RESULT.md](static_ip_m1_20261103/stage0/D1_20261104/V1_SELFTEST_RESULT.md)
- [D1_DEVICE_TEST_APPLICATION.md](static_ip_m1_20261103/stage0/D1_DEVICE_TEST_APPLICATION.md)
- [REVIEW_INDEX.md](static_ip_m1_20261103/stage0/REVIEW_INDEX.md)

### T4a 设备侧记录

- [t4a_device_side_20261102.md](t4a_device_side_20261102.md)
