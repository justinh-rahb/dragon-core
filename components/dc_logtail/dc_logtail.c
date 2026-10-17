#include "dc_logtail.h"

#include "dc_evlog.h"
#include "dc_logtail_contract.h"

#include <string.h>

// SELF-OBSERVATION RULE: nothing in this file may call ESP_LOGx (or anything
// that does on a routine path, such as httpd_resp_send_err). Console output
// feeds the very ring this handler exports, so logging a poll would create
// evidence merely because somebody looked at it.

typedef struct {
    bool latched;
    char boot_id[DC_LOGTAIL_BOOT_ID_MAX + 1];
    dc_logtail_auth_mode_t auth_mode;
    dc_logtail_authorize_fn authorize;
    void *auth_ctx;
} logtail_state_t;

// Written once by the first successful dc_logtail_register() before the route
// exists; read-only afterwards. Request buffers live on the handler's stack,
// so concurrent servers or requests share nothing mutable.
static logtail_state_t s_state;

static esp_err_t emit(httpd_req_t *req, const dc_logtail_response_t *resp,
                      const char *payload)
{
    httpd_resp_set_status(req, resp->status);
    httpd_resp_set_type(req, resp->content_type);
    for (size_t i = 0; i < resp->header_count; ++i) {
        // Fails only when the server's max_resp_headers is too small. Better an
        // explicit, silent 500 than a response missing contract metadata.
        if (httpd_resp_set_hdr(req, resp->headers[i].name,
                               resp->headers[i].value) != ESP_OK) {
            dc_logtail_response_t fail;
            dc_logtail_build_read_response(DC_EVLOG_CONSOLE_READ_INVALID_ARG,
                                           NULL, NULL, &fail);
            httpd_resp_set_status(req, fail.status);
            httpd_resp_set_type(req, fail.content_type);
            return httpd_resp_send(req, fail.body, (ssize_t)fail.body_len);
        }
    }
    // Best effort: the eight contract headers exactly fill ESP-IDF's default
    // max_resp_headers (8). Servers configured with more slots also get
    // no-store; at the default this fails silently and changes nothing else.
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    const char *body = resp->body != NULL ? resp->body : payload;
    return httpd_resp_send(req, body, (ssize_t)resp->body_len);
}

static esp_err_t logtail_get(httpd_req_t *req)
{
    dc_logtail_response_t resp;

    bool callback_result = s_state.auth_mode == DC_LOGTAIL_AUTH_CALLBACK &&
                           s_state.authorize(req, s_state.auth_ctx);
    if (!dc_logtail_auth_permits(s_state.auth_mode, callback_result)) {
        dc_logtail_build_error_response(DC_LOGTAIL_ERR_FORBIDDEN, &resp);
        return emit(req, &resp, NULL);
    }

    char query[DC_LOGTAIL_QUERY_MAX + 1];
    uint64_t cursor = 0;
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len > DC_LOGTAIL_QUERY_MAX ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        !dc_logtail_parse_query(query, &cursor)) {
        dc_logtail_build_error_response(DC_LOGTAIL_ERR_BAD_REQUEST, &resp);
        return emit(req, &resp, NULL);
    }

    // The console lock is held only inside dc_evlog_console_read() for the
    // bounded copy; everything below, including socket I/O, runs unlocked.
    char payload[DC_LOGTAIL_MAX_PAYLOAD];
    dc_evlog_console_read_t info;
    dc_evlog_console_read_status_t status =
        dc_evlog_console_read(cursor, payload, sizeof(payload), &info);
    dc_logtail_build_read_response(status, &info, s_state.boot_id, &resp);
    return emit(req, &resp, payload);
}

static bool same_config(const dc_logtail_config_t *config,
                        dc_logtail_auth_mode_t mode)
{
    return strcmp(s_state.boot_id, config->boot_id) == 0 &&
           s_state.auth_mode == mode &&
           s_state.authorize == config->authorize &&
           s_state.auth_ctx == config->auth_ctx;
}

esp_err_t dc_logtail_register(httpd_handle_t server,
                              const dc_logtail_config_t *config)
{
    if (server == NULL || config == NULL) return ESP_ERR_INVALID_ARG;
    if (!dc_logtail_boot_id_valid(config->boot_id)) return ESP_ERR_INVALID_ARG;
    dc_logtail_auth_mode_t mode = dc_logtail_auth_mode(config->authorize != NULL,
                                                       config->allow_unauthenticated);
    if (mode == DC_LOGTAIL_AUTH_INVALID) return ESP_ERR_INVALID_ARG;

    if (s_state.latched) {
        if (!same_config(config, mode)) return ESP_ERR_INVALID_STATE;
    } else {
        strcpy(s_state.boot_id, config->boot_id);   // length validated above
        s_state.auth_mode = mode;
        s_state.authorize = config->authorize;
        s_state.auth_ctx = config->auth_ctx;
        s_state.latched = true;
    }

    const httpd_uri_t route = {
        .uri = DC_LOGTAIL_URI,
        .method = HTTP_GET,
        .handler = logtail_get,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(server, &route);
}
