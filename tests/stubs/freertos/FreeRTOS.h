#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int portMUX_TYPE;

#define pdTRUE 1
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMUX_INITIALIZER_UNLOCKED 0
// Optional lock-count instrumentation for the console-stream host test.
// It does not emulate FreeRTOS scheduling or actual lock contention.
#ifdef DC_CONSOLE_STREAM_COUNT_CRITICAL
extern unsigned dc_console_stream_critical_enters;
extern unsigned dc_console_stream_critical_exits;
#define portENTER_CRITICAL(mux) ((void)(mux), (void)++dc_console_stream_critical_enters)
#define portEXIT_CRITICAL(mux) ((void)(mux), (void)++dc_console_stream_critical_exits)
#else
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#endif
