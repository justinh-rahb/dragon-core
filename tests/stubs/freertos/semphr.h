#pragma once

#include "FreeRTOS.h"

typedef void *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return (SemaphoreHandle_t)1;
}

static inline int xSemaphoreTake(SemaphoreHandle_t lock, TickType_t wait)
{
    (void)lock;
    (void)wait;
    return pdTRUE;
}

static inline void xSemaphoreGive(SemaphoreHandle_t lock)
{
    (void)lock;
}
