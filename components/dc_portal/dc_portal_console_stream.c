#include "dc_portal_console_stream.h"

#include "dc_evlog.h"

static const char SNAPSHOT_ROTATED[] =
    "\n[console snapshot rotated while streaming]\n";

esp_err_t dc_portal_console_stream(void *ctx, dc_portal_console_send_fn send_chunk)
{
    if (send_chunk == NULL) return ESP_ERR_INVALID_ARG;

    char buf[1024];
    dc_evlog_console_view_t view;
    size_t offset = 0;
    size_t n = dc_evlog_console_snapshot_begin(&view, buf, sizeof(buf));

    if (n > 0) {
        esp_err_t err = send_chunk(ctx, buf, n);
        if (err != ESP_OK) return err;
        offset = n;
    }

    while (offset < view.len) {
        if (!dc_evlog_console_snapshot_read(&view, offset, buf, sizeof(buf), &n)) {
            esp_err_t err = send_chunk(ctx, SNAPSHOT_ROTATED,
                                       sizeof(SNAPSHOT_ROTATED) - 1);
            if (err != ESP_OK) return err;
            break;
        }
        if (n == 0) return ESP_FAIL;

        esp_err_t err = send_chunk(ctx, buf, n);
        if (err != ESP_OK) return err;
        offset += n;
    }

    return send_chunk(ctx, NULL, 0);
}
