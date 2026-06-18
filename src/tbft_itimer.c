#include "tbft_itimer.h"
#include "esp_log.h"

static const char *TAG = "tbft_itimer";

static void timer_dispatch(void *arg)
{
    tbft_itimer_t *t = (tbft_itimer_t *)arg;
    t->running = false;
    if (t->cb) {
        t->cb(t->arg);
    }
}

esp_err_t tbft_itimer_init(tbft_itimer_t *t, tbft_timer_cb_t cb,
                           void *arg, const char *name)
{
    t->cb      = cb;
    t->arg     = arg;
    t->running = false;
    t->handle  = NULL;

    const esp_timer_create_args_t args = {
        .callback        = timer_dispatch,
        .arg             = t,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = name,
        .skip_unhandled_events = true,
    };

    esp_err_t err = esp_timer_create(&args, &t->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create(%s) failed: %s", name, esp_err_to_name(err));
    }
    return err;
}

void tbft_itimer_start(tbft_itimer_t *t, int64_t timeout_us)
{
    if (t->handle == NULL) return;
    if (timeout_us <= 0) {
        ESP_LOGE(TAG, "refusing to start timer with non-positive timeout %lld", (long long)timeout_us);
        return;
    }

    /* CRITICAL FIX: Always stop before starting — esp_timer_stop is idempotent.
     * The previous check on t->running was racy: timer_dispatch clears the flag
     * *before* the callback executes, so a concurrent start() could skip the
     * stop and call esp_timer_start_once on an already-active timer, returning
     * ESP_ERR_INVALID_STATE and leaving t->running inconsistent. */
    esp_timer_stop(t->handle);
    t->running   = true;       /* set BEFORE start_once to prevent race
                                 * with callback running=t->false */
    t->period_us = timeout_us;

    esp_err_t err = esp_timer_start_once(t->handle, timeout_us);
    if (err != ESP_OK) {
        t->running = false;
        ESP_LOGE(TAG, "esp_timer_start_once failed: %s", esp_err_to_name(err));
    }
}

void tbft_itimer_stop(tbft_itimer_t *t)
{
    if (t->handle) {
        /* Always stop — idempotent if already stopped */
        esp_timer_stop(t->handle);
        t->running = false;
    }
}

bool tbft_itimer_is_running(const tbft_itimer_t *t)
{
    return t->running;
}

void tbft_itimer_free(tbft_itimer_t *t)
{
    if (t->handle) {
        tbft_itimer_stop(t);
        esp_timer_delete(t->handle);
        t->handle = NULL;
    }
}
