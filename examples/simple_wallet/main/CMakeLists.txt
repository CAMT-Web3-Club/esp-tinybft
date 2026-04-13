idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES spiffs nvs_flash esp_wifi esp_netif esp_timer)

spiffs_create_partition_image(storage ../spiffs_image FLASH_IN_PROJECT)
