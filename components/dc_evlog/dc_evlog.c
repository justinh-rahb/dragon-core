#include "dc_evlog.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static SemaphoreHandle_t s_lock = NULL;
static dc_evlog_entry_t  s_buf[DC_EVLOG_MAX_ENTRIES];
static size_t            s_head = 0;   // next write slot
static size_t            s_count = 0;  // number of valid entries (<= MAX)

// ---- firmware console capture (raw ESP_LOGx byte ring) ----
// Independent of the curated event ring above. A spinlock (not a mutex) guards it
// because the esp_log vprintf hook can be reached from many contexts and must never
// block or assert. Appends and bounded snapshot reads use short critical sections;
// no console lock is ever held across caller or network I/O. We NEVER log from
// inside the hook.
#define CON_LINE  200
static portMUX_TYPE     s_con_mux = portMUX_INITIALIZER_UNLOCKED;
static char             s_con[DC_EVLOG_CONSOLE_BYTES];
static size_t           s_con_head = 0;    // next write index
static bool             s_con_full = false;
static uint64_t         s_con_write_seq = 0; // total bytes appended; guarded by s_con_mux
static vprintf_like_t   s_prev_vprintf = NULL;
static bool             s_con_on = false;

static void con_append(const char *p, int n)
{
    if (n <= 0) return;
    portENTER_CRITICAL(&s_con_mux);
    for (int i = 0; i < n; i++) {
        s_con[s_con_head++] = p[i];
        if (s_con_head >= DC_EVLOG_CONSOLE_BYTES) { s_con_head = 0; s_con_full = true; }
    }
    s_con_write_seq += (uint64_t)n;
    portEXIT_CRITICAL(&s_con_mux);
}

// esp_log hook: tee the formatted line into the ring, then forward to the original
// writer (UART) via a va_copy so the serial console is unchanged.
static int con_vprintf(const char *fmt, va_list ap)
{
    char line[CON_LINE];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    con_append(line, n < (int)sizeof line ? n : (int)sizeof line - 1);
    int r = s_prev_vprintf ? s_prev_vprintf(fmt, ap2) : 0;
    va_end(ap2);
    return r;
}

void dc_evlog_init(void)
{
    if (s_lock != NULL) return;
    s_lock = xSemaphoreCreateMutex();
    // If mutex creation fails we just stay in the "not initialised" state and
    // dc_evlog_add becomes a no-op — good enough for a diagnostic aid.
}

void dc_evlog_add(const char *fmt, ...)
{
    if (s_lock == NULL) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;

    dc_evlog_entry_t *e = &s_buf[s_head];
    e->ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof(e->text), fmt, ap);
    va_end(ap);

    s_head = (s_head + 1) % DC_EVLOG_MAX_ENTRIES;
    if (s_count < DC_EVLOG_MAX_ENTRIES) s_count++;

    xSemaphoreGive(s_lock);
}

size_t dc_evlog_snapshot(dc_evlog_entry_t *out, size_t max)
{
    if (out == NULL || max == 0) return 0;
    if (s_lock == NULL) return 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;

    size_t want = (max < s_count) ? max : s_count;
    // Copy newest-first: walk backward from s_head.
    size_t idx = (s_head == 0) ? (DC_EVLOG_MAX_ENTRIES - 1) : (s_head - 1);
    for (size_t i = 0; i < want; ++i) {
        out[i] = s_buf[idx];
        idx = (idx == 0) ? (DC_EVLOG_MAX_ENTRIES - 1) : (idx - 1);
    }

    xSemaphoreGive(s_lock);
    return want;
}

void dc_evlog_console_init(void)
{
    if (s_con_on) return;
    s_con_on = true;
    s_prev_vprintf = esp_log_set_vprintf(con_vprintf);   // returns the UART writer
}

size_t dc_evlog_console_snapshot(char *out, size_t max)
{
    if (out == NULL || max == 0) return 0;
    size_t cap = max - 1;   // leave room for the NUL
    size_t len = 0;

    portENTER_CRITICAL(&s_con_mux);
    if (s_con_full) {
        // Ring wrapped: oldest byte is at s_con_head. Emit [head..end) then [0..head).
        size_t tail = DC_EVLOG_CONSOLE_BYTES - s_con_head;
        size_t a = tail < cap ? tail : cap;
        memcpy(out, s_con + s_con_head, a);
        len = a;
        size_t b = s_con_head < (cap - len) ? s_con_head : (cap - len);
        memcpy(out + len, s_con, b);
        len += b;
    } else {
        size_t a = s_con_head < cap ? s_con_head : cap;
        memcpy(out, s_con, a);
        len = a;
    }
    portEXIT_CRITICAL(&s_con_mux);

    out[len] = '\0';
    return len;
}

static size_t con_copy_view_locked(const dc_evlog_console_view_t *view,
                                   size_t offset, char *out, size_t max)
{
    if (offset >= view->len || max == 0) return 0;
    size_t want = view->len - offset;
    if (want > max) want = max;

    size_t idx = (view->start + offset) % DC_EVLOG_CONSOLE_BYTES;
    size_t first = DC_EVLOG_CONSOLE_BYTES - idx;
    if (first > want) first = want;
    memcpy(out, s_con + idx, first);
    if (first < want) memcpy(out + first, s_con, want - first);
    return want;
}

size_t dc_evlog_console_snapshot_begin(dc_evlog_console_view_t *view,
                                       char *out, size_t max)
{
    if (view == NULL || out == NULL || max == 0) return 0;

    portENTER_CRITICAL(&s_con_mux);
    view->start = s_con_full ? s_con_head : 0;
    view->len = s_con_full ? DC_EVLOG_CONSOLE_BYTES : s_con_head;
    view->write_seq = s_con_write_seq;
    size_t written = con_copy_view_locked(view, 0, out, max);
    portEXIT_CRITICAL(&s_con_mux);
    return written;
}

bool dc_evlog_console_snapshot_read(const dc_evlog_console_view_t *view,
                                    size_t offset, char *out, size_t max,
                                    size_t *written)
{
    if (written == NULL) return false;
    *written = 0;
    if (view == NULL || out == NULL || max == 0 || offset > view->len) return false;
    if (offset == view->len) return true;

    portENTER_CRITICAL(&s_con_mux);
    uint64_t advanced = s_con_write_seq - view->write_seq;
    // Before the captured ring was full, new bytes first consume the free tail;
    // after that they overwrite the captured snapshot oldest-first. A chunk at
    // `offset` remains coherent iff producer progress has not reached it yet.
    uint64_t overwrite_budget = (uint64_t)(DC_EVLOG_CONSOLE_BYTES - view->len) + offset;
    if (advanced > overwrite_budget) {
        portEXIT_CRITICAL(&s_con_mux);
        return false;
    }
    *written = con_copy_view_locked(view, offset, out, max);
    portEXIT_CRITICAL(&s_con_mux);
    return true;
}
