#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* 测试桩：格式化后可做文本识别（hook 进入/退出、probe 结果）并可选打印 */
void op_test_log(char level, const char *tag, const char *fmt, ...);
#ifdef __cplusplus
}
#endif
#define ESP_LOGE(tag, fmt, ...) op_test_log('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) op_test_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) op_test_log('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do { (void)(tag); } while (0)
#define ESP_LOGV(tag, fmt, ...) do { (void)(tag); } while (0)
