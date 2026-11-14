idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES spiffs nvs_flash esp_wifi esp_netif esp_timer)

# Written in C23 (ESP-IDF >= 6.0 default, docs/en/api-guides/c.rst).
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu23)

spiffs_create_partition_image(storage ../spiffs_image FLASH_IN_PROJECT)
