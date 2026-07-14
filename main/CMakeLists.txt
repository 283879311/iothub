idf_component_register(
    SRCS "app_main.c"
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
        json
        nvs_flash
)

littlefs_create_partition_image(storage ../web FLASH_IN_PROJECT)
