#pragma once

#include <stddef.h>

#include "esp_http_server.h"

void app_status_get_ap_network_strings(char *ip, size_t ip_len, char *gateway, size_t gw_len,
                                       char *netmask, size_t netmask_len);
esp_err_t app_status_send_device_info(httpd_req_t *req);
esp_err_t app_status_send_device_status(httpd_req_t *req);
