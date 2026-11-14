/*
 * Minimal FreeRTOS API for the host simulator (implemented on pthreads in
 * shim_src/sim_rt.c).  Only what the TinyBFT sources and the sim mains use.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "sdkconfig.h"
#include "esp_err.h"

typedef uint32_t     TickType_t;     /* configUSE_16_BIT_TICKS = 0 on ESP32 */
typedef int          BaseType_t;     /* portBASE_TYPE is int on ESP32 ports */
typedef unsigned int UBaseType_t;
typedef uint32_t     configSTACK_DEPTH_TYPE;

#define configTICK_RATE_HZ   CONFIG_FREERTOS_HZ
#define portTICK_PERIOD_MS   ((TickType_t)1000 / configTICK_RATE_HZ)
#define portMAX_DELAY        ((TickType_t)0xffffffffUL)

#define pdFALSE  ((BaseType_t)0)
#define pdTRUE   ((BaseType_t)1)
#define pdFAIL   pdFALSE
#define pdPASS   pdTRUE

/* Same arithmetic as FreeRTOS projdefs.h */
#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(xTimeInMs) \
    ((TickType_t)(((uint64_t)(xTimeInMs) * (uint64_t)configTICK_RATE_HZ) / (uint64_t)1000U))
#endif
#ifndef pdTICKS_TO_MS
#define pdTICKS_TO_MS(xTicks) \
    ((TickType_t)(((uint64_t)(xTicks) * (uint64_t)1000U) / (uint64_t)configTICK_RATE_HZ))
#endif

/* esp_bit_defs.h */
#ifndef BIT0
#define BIT31   0x80000000
#define BIT7    0x00000080
#define BIT6    0x00000040
#define BIT5    0x00000020
#define BIT4    0x00000010
#define BIT3    0x00000008
#define BIT2    0x00000004
#define BIT1    0x00000002
#define BIT0    0x00000001
#endif

/* Critical sections: ESP-IDF spinlocks.  One process-wide recursive mutex. */
typedef struct { int owner_dummy; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
void sim_port_enter_critical(portMUX_TYPE *mux);
void sim_port_exit_critical(portMUX_TYPE *mux);
#define portENTER_CRITICAL(mux)      sim_port_enter_critical(mux)
#define portEXIT_CRITICAL(mux)       sim_port_exit_critical(mux)
#define taskENTER_CRITICAL(mux)      sim_port_enter_critical(mux)
#define taskEXIT_CRITICAL(mux)       sim_port_exit_critical(mux)
