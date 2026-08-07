#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *data;
    size_t len;
    const char *content_type;
    const char *content_encoding;
} dc_ui_asset_t;

// Describe the immutable, gzip-compressed Dragon-family SPA embedded in flash.
// The returned byte span remains valid for the life of the image.
dc_ui_asset_t dc_ui_spa_asset(void);

#ifdef __cplusplus
}
#endif
