/* File-backed nvs.h shim (see shim/nvs.h). */
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char s_namespace[NVS_NS_NAME_MAX_SIZE];

static void key_path(char *out, size_t n, const char *key)
{
    snprintf(out, n, "nvs_%s_%s.bin", s_namespace, key);
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    (void)open_mode;
    const char *off = getenv("SIM_NO_NVS");
    if (off && off[0] == '1') return ESP_ERR_NVS_NOT_INITIALIZED;
    snprintf(s_namespace, sizeof(s_namespace), "%s", namespace_name);
    *out_handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value)
{
    (void)handle;
    char path[64];
    key_path(path, sizeof(path), key);
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NVS_NOT_FOUND;
    size_t n = fread(out_value, sizeof(*out_value), 1, f);
    fclose(f);
    return n == 1 ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    (void)handle;
    char path[64];
    key_path(path, sizeof(path), key);
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    size_t n = fwrite(&value, sizeof(value), 1, f);
    fclose(f);
    return n == 1 ? ESP_OK : ESP_FAIL;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}
