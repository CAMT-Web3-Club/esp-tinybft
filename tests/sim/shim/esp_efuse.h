#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Only referenced by the ESP-NOW parts of tbft_libbyz.c (not built here). */
esp_err_t esp_efuse_mac_get_default(uint8_t *mac);
