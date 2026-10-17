#pragma once

#include <stdbool.h>
#include <stdint.h>

const char *app_get_hostname(void);
bool app_bt_device_name_is_managed(const char *name);
void app_set_managed_bt_device_name(void);
void app_refresh_managed_identity_fields(void);
