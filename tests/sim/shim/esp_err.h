#pragma once

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                    0
#define ESP_FAIL                  -1
#define ESP_ERR_NO_MEM            0x101
#define ESP_ERR_INVALID_ARG       0x102
#define ESP_ERR_INVALID_STATE     0x103
#define ESP_ERR_INVALID_SIZE      0x104
#define ESP_ERR_NOT_FOUND         0x105
#define ESP_ERR_NOT_SUPPORTED     0x106
#define ESP_ERR_TIMEOUT           0x107
#define ESP_ERR_INVALID_RESPONSE  0x108
#define ESP_ERR_INVALID_CRC       0x109
#define ESP_ERR_INVALID_VERSION   0x10A
#define ESP_ERR_INVALID_MAC       0x10B
#define ESP_ERR_NOT_FINISHED      0x10C
#define ESP_ERR_NOT_ALLOWED       0x10D

const char *esp_err_to_name(esp_err_t code);

/* ESP_ERROR_CHECK aborts the board: in the simulator that is an abort of
 * the process (the orchestrator treats it like a crash/reboot). */
[[noreturn]] void sim_error_check_failed(esp_err_t rc, const char *file,
                                         int line, const char *expr);
#define ESP_ERROR_CHECK(x) do {                                         \
        esp_err_t err_rc_ = (x);                                        \
        if (err_rc_ != ESP_OK) {                                        \
            sim_error_check_failed(err_rc_, __FILE__, __LINE__, #x);    \
        }                                                               \
    } while (0)
#define ESP_ERROR_CHECK_WITHOUT_ABORT(x) ({ esp_err_t err_rc_ = (x); err_rc_; })
