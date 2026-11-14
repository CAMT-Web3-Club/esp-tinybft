/*
 * Task watchdog: a subscribed task that does not call esp_task_wdt_reset()
 * for CONFIG_ESP_TASK_WDT_TIMEOUT_S seconds gets the ESP-IDF error report
 * ("Task watchdog got triggered") plus a SIM_WDT line.  No panic, as with
 * ESP_TASK_WDT_PANIC=n (the ESP-IDF default the counter example keeps).
 */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t esp_task_wdt_add(TaskHandle_t task_handle);
esp_err_t esp_task_wdt_reset(void);
esp_err_t esp_task_wdt_delete(TaskHandle_t task_handle);
