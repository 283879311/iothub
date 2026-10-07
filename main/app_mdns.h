#pragma once

/* mDNS 服务:WiFi 启动后注册 _http._tcp,局域网内 http://<hostname>.local 可达,常驻运行 */
void app_mdns_start(void);
