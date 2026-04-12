#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_timer.h"

/* --------------------------------------------------------------------------
 * ITimer — interval/one-shot timer abstraction backed by esp_timer.
 *
 * Usage:
 *   tbft_itimer_t t;
 *   tbft_itimer_init(&t, my_callback, my_arg, "my_timer");
 *   tbft_itimer_start(&t, 5000000);  // 5 s one-shot
 *   ...
 *   tbft_itimer_stop(&t);
 *   tbft_itimer_free(&t);
 * -------------------------------------------------------------------------- */

typedef void (*tbft_timer_cb_t)(void *arg);

typedef struct {
    esp_timer_handle_t  handle;
    tbft_timer_cb_t     cb;
    void               *arg;
    bool                running;
} tbft_itimer_t;

/**
 * Initialise a timer (does not start it).
 * @param t     Timer to initialise
 * @param cb    Callback invoked when the timer fires
 * @param arg   Opaque argument passed to cb
 * @param name  Debug name string (must remain valid for lifetime of timer)
 * @return ESP_OK on success
 */
esp_err_t tbft_itimer_init(tbft_itimer_t *t, tbft_timer_cb_t cb,
                           void *arg, const char *name);

/**
 * Start (or restart) the timer as a one-shot.
 * @param t           Timer
 * @param timeout_us  Timeout in microseconds
 */
void tbft_itimer_start(tbft_itimer_t *t, int64_t timeout_us);

/**
 * Stop the timer if it is running.
 */
void tbft_itimer_stop(tbft_itimer_t *t);

/**
 * Returns true if the timer is currently scheduled.
 */
bool tbft_itimer_is_running(const tbft_itimer_t *t);

/**
 * Free resources held by the timer.  Stops it first if running.
 */
void tbft_itimer_free(tbft_itimer_t *t);
