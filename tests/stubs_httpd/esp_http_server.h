#pragma once

// Minimal host fake of the esp_http_server surface dc_logtail.c uses. It
// mirrors ESP-IDF v5.3 semantics that matter for response integrity:
// set_status / set_type / set_hdr only store pointers, set_hdr fails with
// ESP_ERR_HTTPD_RESP_HDR once max_resp_headers slots are used, installed
// headers cannot be removed, and nothing is transmitted before
// httpd_resp_send(). Used only by tests/dc_logtail_handler_host_test.c.

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "esp_err.h"

#ifndef ESP_ERR_INVALID_STATE
#define ESP_ERR_INVALID_STATE 0x103
#endif
#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif
#define ESP_ERR_HTTPD_BASE        0xb000
#define ESP_ERR_HTTPD_RESP_HDR    (ESP_ERR_HTTPD_BASE + 5)
#define ESP_ERR_HTTPD_RESULT_TRUNC (ESP_ERR_HTTPD_BASE + 6)

typedef void *httpd_handle_t;
typedef struct httpd_req httpd_req_t;

typedef enum { HTTP_GET = 1 } httpd_method_t;

typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *r);
    void *user_ctx;
} httpd_uri_t;

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, ssize_t buf_len);
size_t httpd_req_get_url_query_len(httpd_req_t *r);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len);
