#pragma once

#include "freertos/FreeRTOS.h"

/* Counting semaphore on a mutex + condition variable.  A "mutex" is a
 * binary semaphore that starts given (no priority inheritance, no owner
 * check), which is all the simulated code needs. */
typedef struct sim_sem *SemaphoreHandle_t;

SemaphoreHandle_t sim_sem_create(unsigned max_count, unsigned initial);
void       vSemaphoreDelete(SemaphoreHandle_t sem);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks_to_wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t sem);

#define xSemaphoreCreateMutex()             sim_sem_create(1, 1)
#define xSemaphoreCreateRecursiveMutex()    sim_sem_create(1, 1)
#define xSemaphoreCreateBinary()            sim_sem_create(1, 0)
#define xSemaphoreCreateCounting(max, init) sim_sem_create((max), (init))
#define xSemaphoreGiveFromISR(s, woken)     ((void)(woken), xSemaphoreGive(s))
#define xSemaphoreTakeRecursive(s, t)       xSemaphoreTake((s), (t))
#define xSemaphoreGiveRecursive(s)          xSemaphoreGive(s)
