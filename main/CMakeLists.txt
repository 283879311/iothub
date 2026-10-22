option(APP_BLE_PEER_TEST "Build the A/B four-characteristic central test harness (default OFF)" OFF)
option(APP_OPERATION_SELFTEST "Build G3 operation table/worker selftest (default OFF)" OFF)
option(APP_OPERATION_LIFECYCLE_TEST "Build G3 worker lifecycle/memory measurement (default OFF)" OFF)
option(APP_OPERATION_WORKER_STATIC "Use static FreeRTOS stack/TCB for the G3 worker (default OFF)" OFF)

idf_component_register(
    SRCS "main.c" "auth.c" "ble_cmd.c" "bootstrap.c" "bt.c" "bt_gattc.c" "bt_gattc_nimble.c" "bt_gatts.c" "bt_gatts_nimble.c" "cmd.c" "common.c" "config.c" "config_apply.c" "data_pipe.c" "diag.c" "identity.c" "mem_guard.c" "mqtt.c" "operation.c" "operation_state.c" "ota.c" "prov_pin.c" "state.c" "transport_ble.c" "uart.c" "wifi.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        bootloader_support
        bt
        esp_driver_gpio
        esp_driver_touch_sens
        esp_driver_uart
        esp_event
        esp_netif
        esp_partition
        esp_system
        esp_wifi
        mbedtls
        mqtt
        nvs_flash
)
if(APP_BLE_PEER_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_BLE_PEER_TEST=1)
endif()
if(APP_OPERATION_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_OPERATION_SELFTEST=1)
endif()
if(APP_OPERATION_LIFECYCLE_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_OPERATION_LIFECYCLE_TEST=1)
endif()
if(APP_OPERATION_WORKER_STATIC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_OPERATION_WORKER_STATIC=1)
endif()

