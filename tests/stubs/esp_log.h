#pragma once

#include <stdarg.h>

typedef int (*vprintf_like_t)(const char *fmt, va_list ap);

vprintf_like_t esp_log_set_vprintf(vprintf_like_t func);
