/*
 * esp_timer on a dedicated pthread ("esp_timer task").  Callbacks run on that
 * thread, concurrently with the tasks, as the high-priority esp_timer task
 * preempts application tasks on the single-core ESP32-C3.
 * esp_timer_get_time() is CLOCK_MONOTONIC microseconds since process start,
 * i.e. it starts at 0 on every (re)boot like on hardware.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct esp_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);

typedef enum {
    ESP_TIMER_TASK,
    ESP_TIMER_ISR,
    ESP_TIMER_MAX,
} esp_timer_dispatch_t;

typedef struct {
    esp_timer_cb_t        callback;
    void                 *arg;
    esp_timer_dispatch_t  dispatch_method;
    const char           *name;
    bool                  skip_unhandled_events;
} esp_timer_create_args_t;

esp_err_t esp_timer_create(const esp_timer_create_args_t *create_args,
                           esp_timer_handle_t *out_handle);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period);
esp_err_t esp_timer_restart(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
esp_err_t esp_timer_delete(esp_timer_handle_t timer);
bool      esp_timer_is_active(esp_timer_handle_t timer);
int64_t   esp_timer_get_time(void);

/* Simulator extension: arg of the most recently created timer with this
 * name (nullptr if none).  Lets the sim mains find internal structures
 * (e.g. the tbft_replica_t that owns "tbft_vtimer") without touching src/. */
void *sim_esp_timer_find_arg(const char *name);
