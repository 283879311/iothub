#pragma once

/* AP 模式强制门户:AP 启动后劫持 DNS(所有域名应答为 AP IP),
 * OS 连通性探测端点由 http_core 统一 302 到配置页,手机接入自动弹窗 */
void app_captive_init(void);
