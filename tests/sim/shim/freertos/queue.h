#pragma once

#include "freertos/FreeRTOS.h"

/* Fixed-size item queue (copy semantics), enough for task-context use. */
typedef struct sim_queue *QueueHandle_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
void          vQueueDelete(QueueHandle_t q);
BaseType_t    xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks_to_wait);
BaseType_t    xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks_to_wait);
UBaseType_t   uxQueueMessagesWaiting(QueueHandle_t q);
BaseType_t    xQueueReset(QueueHandle_t q);

#define xQueueSendToBack(q, item, t) xQueueSend((q), (item), (t))
#define errQUEUE_FULL ((BaseType_t)0)
