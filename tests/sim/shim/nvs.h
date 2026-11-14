/* Host shim of ESP-IDF's nvs.h: the few calls the component uses, backed by
 * one small file per key in the node's working directory, so a value
 * survives a process restart as it survives a board reset.
 * SIM_NO_NVS=1 makes nvs_open() fail like an uninitialised NVS partition. */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#define NVS_KEY_NAME_MAX_SIZE        16
#define NVS_NS_NAME_MAX_SIZE         NVS_KEY_NAME_MAX_SIZE

#define ESP_ERR_NVS_BASE             0x1100
#define ESP_ERR_NVS_NOT_INITIALIZED  (ESP_ERR_NVS_BASE + 0x01)
#define ESP_ERR_NVS_NOT_FOUND        (ESP_ERR_NVS_BASE + 0x02)

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle);
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value);
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value);
esp_err_t nvs_commit(nvs_handle_t handle);
void      nvs_close(nvs_handle_t handle);
