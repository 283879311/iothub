option(APP_BLE_PEER_TEST "Build the A/B four-characteristic central test harness (default OFF)" OFF)
option(APP_OPERATION_SELFTEST "Build G3 operation table/worker selftest (default OFF)" OFF)
option(APP_OPERATION_LIFECYCLE_TEST "Build G3 worker lifecycle/memory measurement (default OFF)" OFF)
option(APP_OPERATION_WORKER_STATIC "Use static FreeRTOS stack/TCB for the G3 worker (default OFF)" OFF)
option(APP_C1_TEST_HOOKS "Experiment-only C1 review hooks (cleanup pause + state-machine probe, default OFF)" OFF)
option(APP_WIFI_FAULT_INJECT "Fault-injection hooks (apply/stop fail, connect hold, restart wake fail; default OFF)" OFF)
option(APP_CMD_MQTT_NET_TXN_TEST_BUILD "Enable MQTT network-transaction test build (default OFF)" OFF)
option(APP_DEV_TEST_HELPER "Build CMD-03 device test helper (default OFF)" OFF)
set(APP_DEV_TEST_SCENARIO "none" CACHE STRING "Device test scenario: none|t13|t19|wait200")

if(APP_DEV_TEST_HELPER)
    if(NOT APP_DEV_TEST_SCENARIO MATCHES "^(t13|t19|wait200)$")
        message(FATAL_ERROR "APP_DEV_TEST_HELPER=ON requires APP_DEV_TEST_SCENARIO=t13|t19|wait200")
    endif()
    string(TOUPPER "${APP_DEV_TEST_SCENARIO}" APP_DEV_TEST_SCENARIO_UPPER)
    set(APP_DEV_TEST_SRCS "dev_test_helper.c")
else()
    if(NOT APP_DEV_TEST_SCENARIO STREQUAL "none")
        message(FATAL_ERROR "APP_DEV_TEST_SCENARIO must be 'none' when APP_DEV_TEST_HELPER=OFF")
    endif()
    set(APP_DEV_TEST_SRCS "")
endif()

idf_component_register(
    SRCS "main.c" "auth.c" "auth_token.c" "ble_cmd.c" "bootstrap.c" "bt.c" "bt_gattc_nimble.c" "bt_gatts_nimble.c" "cmd.c" "cmd_context.c" "cmd_policy.c" "cmd_cfg_txn.c" "cmd_v1.c" "cmd_v1_id.c" "cmd_v1_id_json.c" "cmd_net_job.c" "cmd_error.c" "common.c" "config.c" "config_apply.c" "data_pipe.c" "diag.c" "identity.c" "mem_guard.c" "mqtt.c" "netcheck.c" "netcheck_policy.c" "operation.c" "operation_state.c" "ota.c" "prov_pin.c" "state.c" "transport_ble.c" "uart.c" "wifi.c" "wifi_op_owner.c" ${APP_DEV_TEST_SRCS}
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
        lwip
        mbedtls
        mqtt
        nvs_flash
        spi_flash
)
if(APP_BLE_PEER_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_BLE_PEER_TEST=1)
endif()
if(APP_OPERATION_SELFTEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_OPERATION_SELFTEST=1)
endif()
if(APP_C1_TEST_HOOKS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_C1_TEST_HOOKS=1)
endif()
if(APP_WIFI_FAULT_INJECT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_WIFI_FAULT_INJECT=1)
endif()
if(APP_OPERATION_LIFECYCLE_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_OPERATION_LIFECYCLE_TEST=1)
endif()
if(APP_OPERATION_WORKER_STATIC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_OPERATION_WORKER_STATIC=1)
endif()
if(APP_CMD_MQTT_NET_TXN_TEST_BUILD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        APP_CMD_MQTT_NET_TXN_ENABLE=1
        APP_CMD_MQTT_NET_TXN_TEST_BUILD=1)
endif()
if(APP_DEV_TEST_HELPER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        APP_DEV_TEST_HELPER=1
        APP_DEV_TEST_SCENARIO_${APP_DEV_TEST_SCENARIO_UPPER}=1)
endif()

