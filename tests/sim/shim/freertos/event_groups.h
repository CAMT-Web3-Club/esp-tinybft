#pragma once

#include "freertos/FreeRTOS.h"

typedef TickType_t EventBits_t;
typedef struct sim_event_group *EventGroupHandle_t;

EventGroupHandle_t xEventGroupCreate(void);
void        vEventGroupDelete(EventGroupHandle_t group);
EventBits_t xEventGroupSetBits(EventGroupHandle_t group, const EventBits_t bits);
EventBits_t xEventGroupClearBits(EventGroupHandle_t group, const EventBits_t bits);
EventBits_t xEventGroupGetBits(EventGroupHandle_t group);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group,
                                const EventBits_t bits_to_wait_for,
                                const BaseType_t clear_on_exit,
                                const BaseType_t wait_for_all_bits,
                                TickType_t ticks_to_wait);
