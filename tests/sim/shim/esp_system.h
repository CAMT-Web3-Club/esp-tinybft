#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Exit codes the orchestrator understands. */
#define SIM_EXIT_RESTART      86   /* esp_restart(): board reboot requested */
#define SIM_EXIT_TASK_RETURN  87   /* a FreeRTOS task function returned     */
#define SIM_EXIT_ERROR_CHECK  88   /* ESP_ERROR_CHECK failed                 */

/* Flushes the log and exits the process with SIM_EXIT_RESTART. */
[[noreturn]] void esp_restart(void);

/* A simulated 200 kB heap minus what malloc currently has in use (glibc
 * mallinfo2), so a leak still shows up as a shrinking "heap" field. */
uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);

typedef enum {
    ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC,
    ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT, ESP_RST_SDIO,
} esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);
