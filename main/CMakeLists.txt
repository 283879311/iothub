option(APP_BLE_PEER_TEST "Build the A/B four-characteristic central test harness (default OFF)" OFF)

idf_component_register(
    SRCS "main.c" "app_captive.c" "ble_cmd.c" "ble_prov_probe.c" "bootstrap.c" "bt.c" "bt_gattc.c" "bt_gattc_nimble.c" "bt_gatts.c" "bt_gatts_nimble.c" "cmd.c" "common.c" "config.c" "config_apply.c" "data_pipe.c" "diag.c" "form_url.c" "http_core.c" "http_io.c" "http_mqtt.c" "http_network.c" "http_ota.c" "http_uart.c" "http_utils.c" "identity.c" "json_escape.c" "mem_guard.c" "mqtt.c" "ota.c" "state.c" "status.c" "transport_ble.c" "uart.c" "uart_console.c" "wifi.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        bootloader_support
        bt
        esp_driver_gpio
        esp_driver_touch_sens
        esp_driver_uart
        esp_event
        esp_http_server
        esp_netif
        esp_partition
        esp_system
        esp_wifi
        littlefs
        mbedtls
        mqtt
        nvs_flash
)
if(APP_BLE_PEER_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_BLE_PEER_TEST=1)
endif()

set(APP_WEB_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../web")
set(APP_WEB_MINIFIED_DIR "${CMAKE_BINARY_DIR}/web_minified")
set(APP_WEB_MINIFY_STAMP "${APP_WEB_MINIFIED_DIR}/.minified.stamp")
set(APP_WEB_JS_FILES
    "${APP_WEB_SOURCE_DIR}/config-i18n.js"
    "${APP_WEB_SOURCE_DIR}/config-i18n-en.js"
    "${APP_WEB_SOURCE_DIR}/config-core.js"
    "${APP_WEB_SOURCE_DIR}/config-api.js"
    "${APP_WEB_SOURCE_DIR}/config-panels.js"
    "${APP_WEB_SOURCE_DIR}/config-boot.js"
)

add_custom_command(
    OUTPUT "${APP_WEB_MINIFY_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${APP_WEB_MINIFIED_DIR}"
    COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/../tools/check_web_js.py" "${APP_WEB_SOURCE_DIR}"
    COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/../tools/minify_web.py" "${APP_WEB_SOURCE_DIR}" "${APP_WEB_MINIFIED_DIR}"
    COMMAND ${CMAKE_COMMAND} -E touch "${APP_WEB_MINIFY_STAMP}"
    DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/../tools/check_web_js.py"
        "${CMAKE_CURRENT_LIST_DIR}/../tools/minify_web.py"
        "${APP_WEB_SOURCE_DIR}/config.html"
        ${APP_WEB_JS_FILES}
    COMMENT "Checking and minifying web assets"
    VERBATIM
)

add_custom_target(web_assets_minified DEPENDS "${APP_WEB_MINIFY_STAMP}")

littlefs_create_partition_image(storage "${APP_WEB_MINIFIED_DIR}" FLASH_IN_PROJECT DEPENDS web_assets_minified)
