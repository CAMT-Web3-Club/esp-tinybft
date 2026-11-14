/*
 * esp_log for the host simulator.
 *
 * Output format (one line, stdout, line buffered):
 *   [<run_s>] <L> (<boot_ms>) <tag>: <message>
 * run_s  = seconds since the orchestrator's run start (env SIM_T0_NS,
 *          CLOCK_REALTIME), the same clock in every process;
 * boot_ms = ms since this process started (what ESP-IDF prints).
 *
 * Level filter: env SIM_LOG, e.g. "WARN" (default) or
 * "WARN,tbft_replica=INFO,tbft_state=INFO".  Levels: NONE ERROR WARN INFO
 * DEBUG VERBOSE (or E W I D V).
 *
 * Protocol "event" messages (view changes, fetches, skipped sequence
 * numbers, ...) are matched on their format string and always printed,
 * whatever the level, so the invariant checker can see them without INFO
 * logging (disable with SIM_LOG_EVENTS=0).
 */
#pragma once

#include <stdint.h>
#include <stdarg.h>

typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;

/* cache: per call site, -1 = not classified yet, 0 = normal, 1 = event */
void sim_log_write(signed char *cache, esp_log_level_t level, const char *tag,
                   const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void sim_log_vwrite(signed char *cache, esp_log_level_t level, const char *tag,
                    const char *fmt, va_list ap);
void esp_log_level_set(const char *tag, esp_log_level_t level);
uint32_t esp_log_timestamp(void);

#define ESP_LOG_LEVEL_LOCAL(level, tag, format, ...) do {                \
        static signed char sim_log_site_ = -1;                           \
        sim_log_write(&sim_log_site_, (level), (tag), format             \
                      __VA_OPT__(,) __VA_ARGS__);                        \
    } while (0)
#define ESP_LOG_LEVEL(level, tag, format, ...) \
    ESP_LOG_LEVEL_LOCAL(level, tag, format __VA_OPT__(,) __VA_ARGS__)

#define ESP_LOGE(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_ERROR,   tag, format __VA_OPT__(,) __VA_ARGS__)
#define ESP_LOGW(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_WARN,    tag, format __VA_OPT__(,) __VA_ARGS__)
#define ESP_LOGI(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_INFO,    tag, format __VA_OPT__(,) __VA_ARGS__)
#define ESP_LOGD(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG,   tag, format __VA_OPT__(,) __VA_ARGS__)
#define ESP_LOGV(tag, format, ...) ESP_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, tag, format __VA_OPT__(,) __VA_ARGS__)

#define ESP_EARLY_LOGE ESP_LOGE
#define ESP_EARLY_LOGW ESP_LOGW
#define ESP_EARLY_LOGI ESP_LOGI
#define ESP_EARLY_LOGD ESP_LOGD
#define ESP_EARLY_LOGV ESP_LOGV
#define ESP_DRAM_LOGE  ESP_LOGE
#define ESP_DRAM_LOGW  ESP_LOGW
#define ESP_DRAM_LOGI  ESP_LOGI
