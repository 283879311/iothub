#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

void app_sim_init(void);
esp_err_t app_sim_send_status_json(httpd_req_t *req, const char *message);
esp_err_t app_sim_handle_start_request(httpd_req_t *req, const char *body);
esp_err_t app_sim_handle_exit_request(httpd_req_t *req);
esp_err_t app_sim_handle_stop_request(httpd_req_t *req);
