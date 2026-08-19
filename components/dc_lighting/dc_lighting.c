#include "dc_lighting.h"

#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "led_strip_spi.h"

static const char *TAG = "dc_lighting";
static led_strip_handle_t s_strip[DC_LIGHTING_MAX_OUTPUTS];
static dc_lighting_output_t s_output[DC_LIGHTING_MAX_OUTPUTS];
static uint8_t s_count, s_brightness = 255, s_fps = 30, s_speed;
static uint16_t s_total_pixels;
static dc_lighting_layout_t s_layout;
static float s_progress = -1.0f;
static float s_audio_level;
static dc_rgb_t s_color;
static dc_lighting_effect_t s_effect;
static SemaphoreHandle_t s_lock;

/* Renderer health, written by the render task under s_lock. */
static uint32_t s_frames, s_refresh_errors, s_pixel_errors;
static esp_err_t s_last_error = ESP_OK;
static int64_t s_last_error_us, s_last_error_log_us;
static bool s_failing;

/* The console ring holds ~180 lines, so complaining once per frame would erase
 * the boot context that makes a failure diagnosable. Log transitions instead,
 * and leave the continuous view to the counters. */
#define DC_LIGHTING_ERROR_LOG_INTERVAL_US (30LL * 1000000LL)

static uint8_t scale(uint8_t value, uint8_t level) { return (uint16_t)value * level / 255; }

/* Speed selects a rate rather than multiplying an arbitrary phase: this avoids
 * phase aliasing which otherwise makes a Breathe effect look like a flash. */
static uint32_t animation_phase(uint32_t tick) { return tick * (1 + s_speed / 32); }

static void hsv(uint16_t hue, dc_rgb_t *out)
{
    uint8_t region = hue / 10923;
    uint16_t rem = (uint16_t)((hue - region * 10923) * 6);
    uint8_t q = 255 - rem / 257;
    uint8_t t = rem / 257;
    switch (region) {
    case 0: *out = (dc_rgb_t){255, t, 0}; break;
    case 1: *out = (dc_rgb_t){q, 255, 0}; break;
    case 2: *out = (dc_rgb_t){0, 255, t}; break;
    case 3: *out = (dc_rgb_t){0, q, 255}; break;
    case 4: *out = (dc_rgb_t){t, 0, 255}; break;
    default:*out = (dc_rgb_t){255, 0, q}; break;
    }
}

static void frame(uint32_t tick)
{
    uint8_t level = s_brightness;
    uint32_t phase = animation_phase(tick);
    if (s_effect == DC_LIGHTING_BREATHE) {
        uint16_t wave_phase = phase & 511;
        uint8_t wave = wave_phase < 256 ? wave_phase : 511 - wave_phase;
        level = scale(level, 40 + (uint16_t)wave * 215 / 255);
    } else if (s_effect == DC_LIGHTING_STROBE && ((phase / 64) & 1)) {
        level = 0;
    }

    bool pixel_failed = false, refresh_failed = false;
    uint16_t output_offset = 0;
    for (uint8_t i = 0; i < s_count; ++i) {
        for (uint16_t p = 0; p < s_output[i].pixels; ++p) {
            uint16_t position = s_layout == DC_LIGHTING_LAYOUT_CONTIGUOUS ? output_offset + p : p;
            uint16_t pixels = s_layout == DC_LIGHTING_LAYOUT_CONTIGUOUS ? s_total_pixels : s_output[i].pixels;
            dc_rgb_t color = s_color;
            uint8_t pixel_level = level;
            if (s_effect == DC_LIGHTING_CYCLE) {
                hsv((uint16_t)(phase * 128), &color);
            } else if (s_effect == DC_LIGHTING_RAINBOW) {
                hsv((uint16_t)(phase * 128 + position * (65536 / pixels)), &color);
            } else if (s_effect == DC_LIGHTING_FLOW) {
                uint16_t head = (phase / 6) % pixels;
                uint16_t distance = (position + pixels - head) % pixels;
                pixel_level = scale(s_brightness, distance < 6 ? (uint8_t)((6 - distance) * 255 / 6) : 0);
            } else if (s_effect == DC_LIGHTING_WAVE) {
                uint16_t x = (uint16_t)(phase * 128 + position * (65536 / pixels));
                uint8_t wave = x < 32768 ? x >> 7 : 255 - ((x - 32768) >> 7);
                pixel_level = scale(s_brightness, wave);
            } else if (s_effect == DC_LIGHTING_MARQUEE) {
                pixel_level = ((position + phase / 16) % 3) == 0 ? s_brightness : 0;
            } else if (s_effect == DC_LIGHTING_CYLON) {
                uint16_t span = pixels > 1 ? (pixels - 1) * 2 : 1;
                uint16_t travel = (phase / 8) % span;
                uint16_t head = travel < pixels ? travel : span - travel;
                uint16_t distance = position > head ? position - head : head - position;
                pixel_level = scale(s_brightness, distance < 5 ? (uint8_t)((5 - distance) * 255 / 5) : 0);
            } else if (s_effect == DC_LIGHTING_PROGRESS) {
                bool lit = s_progress >= 0.0f && position < (uint16_t)(s_progress * pixels);
                color = lit ? s_color : (dc_rgb_t){0, 0, 0};
                pixel_level = lit ? s_brightness : 255;
            } else if (s_effect == DC_LIGHTING_AUDIO_METER) {
                float meter = s_audio_level;
                uint16_t lit = meter <= 0.0f ? 0 : (uint16_t)(meter * pixels + 0.999f);
                bool on = position < lit;
                /* HSV runs red -> yellow -> green -> cyan -> blue. Reverse
                 * it so a quiet room is blue and a loud one is red. */
                hsv((uint16_t)((1.0f - meter) * 43690.0f), &color);
                pixel_level = on ? s_brightness : 0;
            }
            uint16_t index = s_output[i].reverse ? s_output[i].pixels - 1 - p : p;
            esp_err_t err = led_strip_set_pixel(s_strip[i], index, scale(color.r, pixel_level), scale(color.g, pixel_level), scale(color.b, pixel_level));
            if (err != ESP_OK && !pixel_failed) { pixel_failed = true; s_last_error = err; }
        }
        esp_err_t err = led_strip_refresh(s_strip[i]);
        if (err != ESP_OK && !refresh_failed) { refresh_failed = true; s_last_error = err; }
        output_offset += s_output[i].pixels;
    }

    ++s_frames;
    if (pixel_failed) ++s_pixel_errors;
    if (refresh_failed) ++s_refresh_errors;
    if (pixel_failed || refresh_failed) {
        int64_t now = esp_timer_get_time();
        s_last_error_us = now;
        /* First failure is always reported; a persistent one repeats slowly so
         * the ring keeps both the onset and the surrounding context. */
        if (!s_failing || now - s_last_error_log_us >= DC_LIGHTING_ERROR_LOG_INTERVAL_US) {
            ESP_LOGW(TAG, "strip output failing: %s (frame %lu, refresh_err=%lu pixel_err=%lu)",
                     esp_err_to_name(s_last_error), (unsigned long)s_frames,
                     (unsigned long)s_refresh_errors, (unsigned long)s_pixel_errors);
            s_last_error_log_us = now;
        }
        s_failing = true;
    } else if (s_failing) {
        ESP_LOGI(TAG, "strip output recovered at frame %lu (refresh_err=%lu pixel_err=%lu)",
                 (unsigned long)s_frames, (unsigned long)s_refresh_errors, (unsigned long)s_pixel_errors);
        s_failing = false;
    }
}

void dc_lighting_get_stats(dc_lighting_stats_t *out)
{
    if (!out) return;
    if (!s_lock) { *out = (dc_lighting_stats_t){ .last_error = ESP_OK }; return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = (dc_lighting_stats_t){
        .frames = s_frames,
        .refresh_errors = s_refresh_errors,
        .pixel_errors = s_pixel_errors,
        .last_error = s_last_error,
        .last_error_us = s_last_error_us,
        .failing = s_failing,
        .color = s_color,
        .effect = (uint8_t)s_effect,
        .brightness = s_brightness,
    };
    xSemaphoreGive(s_lock);
}

static void render_task(void *arg)
{
    (void)arg;
    for (uint32_t tick = 0;; ++tick) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        frame(tick);
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(1000 / s_fps));
    }
}

esp_err_t dc_lighting_start(const dc_lighting_config_t *config)
{
    if (!config || !config->outputs || !config->output_count || config->output_count > DC_LIGHTING_MAX_OUTPUTS || !config->fps) return ESP_ERR_INVALID_ARG;
    if (s_lock) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_brightness = config->brightness;
    s_fps = config->fps;
    s_layout = config->layout;
    for (uint8_t i = 0; i < config->output_count; ++i) {
        if (config->outputs[i].gpio == GPIO_NUM_NC || !config->outputs[i].pixels) return ESP_ERR_INVALID_ARG;
        led_strip_config_t strip = { .strip_gpio_num = config->outputs[i].gpio, .max_leds = config->outputs[i].pixels, .led_pixel_format = LED_PIXEL_FORMAT_GRB, .led_model = LED_MODEL_WS2812 };
        esp_err_t err;
        if (config->outputs[i].transport == DC_LIGHTING_TRANSPORT_SPI_DMA) {
            led_strip_spi_config_t spi = { .clk_src = SPI_CLK_SRC_DEFAULT, .spi_bus = config->outputs[i].spi_host, .flags = { .with_dma = true } };
            err = led_strip_new_spi_device(&strip, &spi, &s_strip[i]);
        } else {
            led_strip_rmt_config_t rmt = { .resolution_hz = 10 * 1000 * 1000 };
            err = led_strip_new_rmt_device(&strip, &rmt, &s_strip[i]);
        }
        if (err != ESP_OK) return err;
        s_output[i] = config->outputs[i];
        s_total_pixels += config->outputs[i].pixels;
        ++s_count;
    }
    ESP_LOGI(TAG, "started %u WS2812 output(s)", s_count);
    return xTaskCreate(render_task, "dc_lighting", 3072, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t dc_lighting_set(dc_rgb_t color, dc_lighting_effect_t effect, uint8_t speed)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_color = color; s_effect = effect; s_speed = speed;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t dc_lighting_set_brightness(uint8_t brightness)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY); s_brightness = brightness; xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t dc_lighting_set_output_reverse(uint8_t output, bool reverse)
{
    if (!s_lock || output >= s_count) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY); s_output[output].reverse = reverse; xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t dc_lighting_set_progress(float progress)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (progress > 1.0f) progress = 1.0f;
    xSemaphoreTake(s_lock, portMAX_DELAY); s_progress = progress; xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t dc_lighting_set_audio_level(float level)
{
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    xSemaphoreTake(s_lock, portMAX_DELAY); s_audio_level = level; xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t dc_lighting_off(void) { return dc_lighting_set((dc_rgb_t){0, 0, 0}, DC_LIGHTING_SOLID, 0); }
