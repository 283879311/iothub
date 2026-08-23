idf_component_register(
    SRCS "main.c" "bootstrap.c" "bt.c" "common.c" "config.c" "http_core.c" "http_io.c" "http_mqtt.c" "http_network.c" "http_ota.c" "http_uart.c" "http_utils.c" "identity.c" "mqtt.c" "ota.c" "sim.c" "state.c" "status.c" "uart.c" "uart_console.c" "wifi.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        bootloader_support
        bt
        esp_driver_gpio
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

set(APP_WEB_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../web")
set(APP_WEB_MINIFIED_DIR "${CMAKE_BINARY_DIR}/web_minified")
set(APP_WEB_MINIFY_STAMP "${APP_WEB_MINIFIED_DIR}/.minified.stamp")

add_custom_command(
    OUTPUT "${APP_WEB_MINIFY_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${APP_WEB_MINIFIED_DIR}"
    COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/../tools/check_web_js.py" "${APP_WEB_SOURCE_DIR}"
    COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/../tools/minify_web.py" "${APP_WEB_SOURCE_DIR}" "${APP_WEB_MINIFIED_DIR}"
    COMMAND ${CMAKE_COMMAND} -E touch "${APP_WEB_MINIFY_STAMP}"
    DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/../tools/check_web_js.py"
        "${CMAKE_CURRENT_LIST_DIR}/../tools/minify_web.py"
        "${CMAKE_CURRENT_LIST_DIR}/../web/index.html"
        "${CMAKE_CURRENT_LIST_DIR}/../web/config.html"
    COMMENT "Checking and minifying web assets"
    VERBATIM
)

add_custom_target(web_assets_minified DEPENDS "${APP_WEB_MINIFY_STAMP}")

littlefs_create_partition_image(storage "${APP_WEB_MINIFIED_DIR}" FLASH_IN_PROJECT DEPENDS web_assets_minified)
