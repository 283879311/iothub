from pathlib import Path

ROOT = Path(__file__).resolve().parent
APP_MAIN = ROOT / "main" / "app_main.c"
WEB_INDEX = ROOT / "web" / "index.html"


def _app_source() -> str:
    return APP_MAIN.read_text(encoding="utf-8")


def _function_body(source: str, name: str) -> str:
    signature = f"static esp_err_t {name}(httpd_req_t *req)"
    start = source.index(signature)
    body_start = source.index("{", start)
    depth = 0
    for index in range(body_start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[body_start:index + 1]
    raise AssertionError(f"function {name} not closed")


def test_management_api_handlers_require_auth() -> None:
    source = _app_source()
    protected_handlers = [
        "network_get_handler",
        "network_put_handler",
        "network_restart_handler",
        "network_scan_handler",
        "gpio_get_handler",
        "gpio_put_handler",
        "bluetooth_get_handler",
        "bluetooth_put_handler",
        "mqtt_get_handler",
        "mqtt_put_handler",
        "uart_get_handler",
        "uart_put_handler",
        "uart_probe_auto_detect_handler",
        "uart_probe_manual_detect_handler",
        "uart_probe_confirm_handler",
        "uart_probe_stop_handler",
        "uart_console_get_handler",
        "uart_send_handler",
        "ota_upload_handler",
    ]

    for handler in protected_handlers:
        assert "APP_REQUIRE_AUTH(req);" in _function_body(source, handler), handler


def test_web_ui_logs_in_and_sends_auth_header() -> None:
    html = WEB_INDEX.read_text(encoding="utf-8")

    assert 'fetch("/api/v1/auth/login"' in html
    assert 'headers.set("X-IoTHub-Auth", state.authToken);' in html
    assert 'localStorage.setItem("iothubAuthToken", body.token);' in html
    assert "response.status === 401" in html


def test_mqtt_rpc_commands_are_parsed_with_cjson() -> None:
    source = _app_source()
    body = source[source.index("static void app_mqtt_apply_command"):source.index("static void app_mqtt_event_handler")]

    assert "cJSON_Parse(payload)" in body
    assert "cJSON_GetObjectItemCaseSensitive(root, \"method\")" in body
    assert "strstr(payload" not in body


def test_config_persistence_uses_key_value_nvs_not_current_blob_write() -> None:
    source = _app_source()
    save_body = source[source.index("static esp_err_t app_save_config_to_nvs"):source.index("static void app_migrate_config_v3")]

    assert "nvs_set_u16(nvs_handle, APP_CONFIG_VERSION_KEY" in save_body
    assert "nvs_set_str" in source
    assert "nvs_set_blob" not in save_body
    assert "Migrating saved config from v5 blob to key-value NVS" in source
