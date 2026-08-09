// SPDX-License-Identifier: MIT
#include "dc_portal_dns.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define DNS_PORT 53
#define DNS_MAX_LEN 512

static const char *TAG = "dc_portal_dns";
static volatile TaskHandle_t s_task;
static int s_sock = -1;
static uint32_t s_redirect_ip;
static volatile bool s_running;

typedef struct __attribute__((packed)) {
    uint16_t id, flags, qdcount, ancount, nscount, arcount;
} dns_hdr_t;

static size_t qname_length(const uint8_t *buf, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t n = buf[i];
        if (n == 0) return i + 1;
        if (n >= 64) return 0;
        i += n + 1;
    }
    return 0;
}

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t buf[DNS_MAX_LEN];
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(DNS_PORT) };
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0 || bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "socket/bind failed");
        if (s_sock >= 0) close(s_sock);
        s_sock = -1;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    while (s_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(s_sock, &read_fds);
        struct timeval timeout = { .tv_sec = 0, .tv_usec = 200000 };
        int ready = select(s_sock + 1, &read_fds, NULL, NULL, &timeout);
        if (!s_running) break;
        if (ready <= 0) continue;
        struct sockaddr_in src = {0};
        socklen_t src_len = sizeof(src);
        int received = recvfrom(s_sock, buf, sizeof(buf), 0,
                                (struct sockaddr *)&src, &src_len);
        if (received < (int)sizeof(dns_hdr_t)) continue;
        dns_hdr_t *hdr = (dns_hdr_t *)buf;
        if ((ntohs(hdr->flags) & 0x8000) || ntohs(hdr->qdcount) == 0) continue;
        size_t off = sizeof(*hdr);
        size_t qn = qname_length(buf + off, (size_t)received - off);
        if (!qn || off + qn + 4 > (size_t)received) continue;
        if (buf[off + qn] != 0 || buf[off + qn + 1] != 1 ||
            buf[off + qn + 2] != 0 || buf[off + qn + 3] != 1) continue;
        hdr->flags = htons(0x8180);
        hdr->qdcount = htons(1);
        hdr->ancount = htons(1);
        hdr->nscount = hdr->arcount = 0;
        size_t response_len = off + qn + 4;
        uint8_t answer[] = {
            0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4,
            (uint8_t)s_redirect_ip, (uint8_t)(s_redirect_ip >> 8),
            (uint8_t)(s_redirect_ip >> 16), (uint8_t)(s_redirect_ip >> 24),
        };
        if (response_len + sizeof(answer) > sizeof(buf)) continue;
        memcpy(buf + response_len, answer, sizeof(answer));
        sendto(s_sock, buf, response_len + sizeof(answer), 0,
               (struct sockaddr *)&src, src_len);
    }
    close(s_sock);
    s_sock = -1;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t dc_portal_dns_start(uint32_t redirect_ip)
{
    if (s_task) return ESP_ERR_INVALID_STATE;
    s_redirect_ip = redirect_ip;
    s_running = true;
    if (xTaskCreate(dns_task, "dc_portal_dns", 4096, NULL, 4,
                    (TaskHandle_t *)&s_task) == pdPASS) return ESP_OK;
    s_running = false;
    return ESP_ERR_NO_MEM;
}

void dc_portal_dns_stop(void)
{
    s_running = false;
    // The task owns its socket and exits itself. Its bounded select timeout
    // avoids deleting a task while lwIP may hold internal state in recvfrom().
    for (int i = 0; s_task && i < 20; ++i)
        vTaskDelay(pdMS_TO_TICKS(25));
    if (s_task) ESP_LOGW(TAG, "DNS task did not stop within 500 ms");
}
