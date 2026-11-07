#include "cmd_error.h"

#include <stddef.h>
#include <string.h>

const char *app_cmd_v1_error_code(const char *v0)
{
    if (v0 == NULL) {
        return "internal_error";
    }
    if (strcmp(v0, "unknown_command") == 0 ||
        strcmp(v0, "unknown_method") == 0) {
        return "unknown_command";
    }
    if (strcmp(v0, "line_too_long") == 0) {
        return "line_too_long";
    }
    if (strcmp(v0, "invalid_json") == 0) {
        return "invalid_json";
    }
    if (strcmp(v0, "unsupported_version") == 0) {
        return "unsupported_version";
    }
    if (strcmp(v0, "params_required") == 0 ||
        strcmp(v0, "invalid_config") == 0 ||
        strcmp(v0, "invalid_param") == 0 ||
        strcmp(v0, "invalid_argument") == 0 ||
        strcmp(v0, "invalid_old_password") == 0 ||
        strcmp(v0, "domain_not_supported") == 0 ||
        strcmp(v0, "password_too_short") == 0) {
        return "invalid_argument";
    }
    if (strcmp(v0, "auth_required") == 0 ||
        strcmp(v0, "invalid_credentials") == 0 ||
        strcmp(v0, "unauthorized") == 0) {
        return "unauthorized";
    }
    if (strcmp(v0, "guard_rejected") == 0 ||
        strcmp(v0, "forbidden") == 0) {
        return "forbidden";
    }
    if (strcmp(v0, "busy") == 0) {
        return "busy";
    }
    if (strcmp(v0, "conflict") == 0) {
        return "conflict";
    }
    if (strcmp(v0, "rate_limited") == 0) {
        return "rate_limited";
    }
    if (strcmp(v0, "internal_error") == 0) {
        return "internal_error";
    }
    if (strcmp(v0, "mqtt_unavailable") == 0 ||
        strcmp(v0, "internet_unverified") == 0 ||
        strcmp(v0, "verify_timeout") == 0 ||
        strcmp(v0, "static_ip_pinned") == 0 ||
        strcmp(v0, "last_profile") == 0 ||
        strcmp(v0, "wifi_config_offline") == 0 ||
        strcmp(v0, "connect_timeout") == 0 ||
        strcmp(v0, "cancelled") == 0) {
        return v0;
    }
    if (strcmp(v0, "not_found") == 0 ||
        strcmp(v0, "operation_state") == 0 ||
        strcmp(v0, "trial_invalid") == 0 ||
        strcmp(v0, "wrong_password") == 0 ||
        strcmp(v0, "ap_not_found") == 0 ||
        strcmp(v0, "connect_failed") == 0 ||
        strcmp(v0, "wifi_scan_failed") == 0 ||   /* P1-10:保持可分辨,不折叠进 network_failed */
        strcmp(v0, "no_slot") == 0 ||
        strcmp(v0, "expired") == 0 ||
        strcmp(v0, "rollback_failed") == 0) {
        return v0;
    }
    if (strcmp(v0, "save_failed") == 0 ||
        strcmp(v0, "storage_failed") == 0) {
        return "storage_failed";
    }
    if (strcmp(v0, "wifi_scan_failed") == 0 ||
        strcmp(v0, "network_failed") == 0) {
        return "network_failed";
    }
    if (strcmp(v0, "restart_failed") == 0 ||
        strcmp(v0, "apply_failed") == 0) {
        return "apply_failed";
    }
    return "internal_error";
}

