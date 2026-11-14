#pragma once

#include "freertos/FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);
typedef struct sim_task *TaskHandle_t;

/* Creates a pthread.  As in ESP-IDF (FREERTOS_TASK_FUNCTION_WRAPPER), a task
 * function that returns aborts the "board" (the simulated process exits). */
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
                       configSTACK_DEPTH_TYPE stack_depth, void *arg,
                       UBaseType_t priority, TaskHandle_t *created);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
                                   configSTACK_DEPTH_TYPE stack_depth,
                                   void *arg, UBaseType_t priority,
                                   TaskHandle_t *created, BaseType_t core);
void       vTaskDelete(TaskHandle_t task);
void       vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
const char *pcTaskGetName(TaskHandle_t task);
void       sim_task_yield(void);
#define taskYIELD() sim_task_yield()
