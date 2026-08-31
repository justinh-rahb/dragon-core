#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int portMUX_TYPE;

#define pdTRUE 1
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
