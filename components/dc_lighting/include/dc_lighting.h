#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

/* Board-neutral WS2812 renderer. Products own GPIO topology, persistence, and
 * state policy; this component owns strip transport, frame timing, brightness,
 * direction, and effects. Effect values 0..6 are stable for DragonStatus. */

#define DC_LIGHTING_MAX_OUTPUTS 2

typedef enum {
    DC_LIGHTING_SOLID = 0,
    DC_LIGHTING_BREATHE = 1,
    DC_LIGHTING_RAINBOW = 2,
    DC_LIGHTING_STROBE = 3,
    DC_LIGHTING_FLOW = 4,
    DC_LIGHTING_PROGRESS = 5,
    DC_LIGHTING_CYLON = 6,
    DC_LIGHTING_CYCLE = 7,
    DC_LIGHTING_WAVE = 8,
    DC_LIGHTING_MARQUEE = 9,
    /* A level meter: length and hue follow a normalized audio level. */
    DC_LIGHTING_AUDIO_METER = 10,
} dc_lighting_effect_t;

/* Compatibility spelling for products which called Strobe "blink". */
#define DC_LIGHTING_BLINK DC_LIGHTING_STROBE

typedef enum {
    DC_LIGHTING_TRANSPORT_RMT = 0,
    DC_LIGHTING_TRANSPORT_SPI_DMA,
} dc_lighting_transport_t;

/* Spatial effects may repeat on each physical output or span their concatenated
 * logical pixel line. Each product picks the layout that matches its hardware. */
typedef enum {
    DC_LIGHTING_LAYOUT_PER_OUTPUT = 0,
    DC_LIGHTING_LAYOUT_CONTIGUOUS,
} dc_lighting_layout_t;

typedef struct { uint8_t r, g, b; } dc_rgb_t;

typedef struct {
    gpio_num_t gpio;
    uint16_t pixels;
    bool reverse;
    dc_lighting_transport_t transport;
    int spi_host;                 /* SPI2_HOST/SPI3_HOST for SPI+DMA. */
} dc_lighting_output_t;

typedef struct {
    const dc_lighting_output_t *outputs;
    uint8_t output_count;
    uint8_t brightness;
    uint8_t fps;
    dc_lighting_layout_t layout;
} dc_lighting_config_t;

/* Renderer health. The strip driver can fail on every frame while the render
 * task keeps looping, which from outside is indistinguishable from correctly
 * rendering a black frame: the product reports a healthy config, the mutex
 * cycles normally, and the bar stays dark with nothing logged anywhere. These
 * counters make that state observable, including over a product's HTTP API
 * when no serial console is attached. */
typedef struct {
    uint32_t frames;           /* frames rendered since start */
    uint32_t refresh_errors;   /* frames where led_strip_refresh() failed */
    uint32_t pixel_errors;     /* frames where a pixel write failed */
    esp_err_t last_error;      /* most recent driver error; ESP_OK if none yet */
    int64_t last_error_us;     /* esp_timer stamp of that error, 0 if none */
    bool failing;              /* the most recent frame did not render cleanly */
    dc_rgb_t color;            /* colour the renderer is currently painting */
    uint8_t effect;
    uint8_t brightness;
} dc_lighting_stats_t;

esp_err_t dc_lighting_start(const dc_lighting_config_t *config);
/* Safe before dc_lighting_start(): reports a zeroed snapshot. */
void dc_lighting_get_stats(dc_lighting_stats_t *out);
esp_err_t dc_lighting_set(dc_rgb_t color, dc_lighting_effect_t effect, uint8_t speed);
esp_err_t dc_lighting_set_brightness(uint8_t brightness);
esp_err_t dc_lighting_set_output_reverse(uint8_t output, bool reverse);
/* Progress is normalized to 0..1. A negative value means unavailable. */
esp_err_t dc_lighting_set_progress(float progress);
/* Audio level is normalized to 0..1 and consumed by DC_LIGHTING_AUDIO_METER. */
esp_err_t dc_lighting_set_audio_level(float level);
esp_err_t dc_lighting_off(void);
