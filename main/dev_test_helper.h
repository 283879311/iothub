#pragma once

#include "esp_err.h"

#ifdef APP_DEV_TEST_HELPER

/* CMD-03 设备测试辅助器（仅测试构建）。
 * 场景由 APP_DEV_TEST_SCENARIO_T13/T19/WAIT200 编译期选择；
 * 启动后自行等待网络就绪并通过真实 operation 受理门创建测试 operation。
 * 不修改生产状态机、锁序、cleanup claim 或 owner/defer 语义。 */
esp_err_t app_dev_test_helper_start(void);

#endif
