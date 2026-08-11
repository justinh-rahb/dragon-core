// Bambu LAN discovery over SSDP, on demand only. See dc_bambu.h for the wire
// protocol.
//
// Design: discovery is NEVER continuous. There is no background listener and no
// socket held open — a scan is a short-lived task the UI kicks off only while the
// operator is configuring a Bambu printer. The task opens ONE UDP socket on port
// 2021, joins the multicast group on the STA interface, and for a few seconds
// both listens for unsolicited NOTIFY (real printers, ~5 s cadence) and re-sends
// an M-SEARCH probe (emulators/quick replies come back to our port). NOTIFY and
// the 200 OK reply carry identical headers, so one parser handles both. When the
// window elapses the socket is closed and the task exits, freeing the socket back
// to the small LWIP pool the single-worker httpd shares.

#include "dc_bambu.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <string.h>
#include <strings.h>

static const char *TAG = "dc_bambu_disc";

#define SSDP_GROUP     "239.255.255.250"
#define AGE_OUT_US     (120 * 1000000LL)   // drop a printer unseen for 2 min
#define SCAN_WINDOW_US (6 * 1000000LL)     // one scan listens ~6 s (covers a 5 s NOTIFY cycle)
#define PROBE_EVERY_MS 1500                // re-probe a few times within the window

// Bambu SSDP uses ports 1990 AND 2021, but the LWIP socket pool on these devices
// is tiny (shared with a single-worker httpd), so a scan binds ONE receive socket
// on 2021 — the port Bambu Studio/OrcaSlicer use and the emulator answers on, and
// which real printers emit on too. We still PROBE both ports (M-SEARCH replies
// return to our source port regardless), so coverage is unchanged.
#define RX_PORT 2021
static const uint16_t PROBE_PORTS[2] = { 1990, 2021 };

static const char MSEARCH[] =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:2021\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 1\r\n"
    "ST: urn:bambulab-com:device:3dprinter:1\r\n\r\n";

typedef struct {
    dc_bambu_found_t info;   // age_s is filled in at get() time, not stored
    int64_t last_seen_us;
    bool    used;
} slot_t;

static slot_t            s_slots[DC_BAMBU_DISCOVER_MAX];
static SemaphoreHandle_t s_lock;
static volatile bool     s_scanning;

// --- header parsing ---------------------------------------------------------

// Copy the value of header `name` (case-insensitive, matched at a line start)
// into out[cap], trimming leading spaces and stopping at CR/LF. Returns true if
// found. `buf` is the whole datagram (NUL-terminated by the caller).
static bool header(const char *buf, const char *name, char *out, size_t cap)
{
    size_t nlen = strlen(name);
    const char *p = buf;
    while (p && *p) {
        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t i = 0;
            while (v[i] && v[i] != '\r' && v[i] != '\n' && i + 1 < cap) { out[i] = v[i]; i++; }
            out[i] = '\0';
            return i > 0;
        }
        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : NULL;
    }
    return false;
}

// Record (or refresh) a printer keyed by serial.
static void remember(const char *serial, const char *host,
                     const char *model, const char *name)
{
    if (!serial[0] || !host[0]) return;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);

    int free_i = -1, oldest_i = -1;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < DC_BAMBU_DISCOVER_MAX; i++) {
        if (s_slots[i].used && strcmp(s_slots[i].info.serial, serial) == 0) {
            free_i = i;   // update in place
            break;
        }
        if (!s_slots[i].used && free_i < 0) free_i = i;
        if (s_slots[i].used && s_slots[i].last_seen_us < oldest) {
            oldest = s_slots[i].last_seen_us; oldest_i = i;
        }
    }
    int i = (free_i >= 0) ? free_i : oldest_i;   // evict oldest if full
    bool is_new = !s_slots[i].used || strcmp(s_slots[i].info.serial, serial) != 0;
    s_slots[i].used = true;
    s_slots[i].last_seen_us = now;
    snprintf(s_slots[i].info.serial, sizeof s_slots[i].info.serial, "%s", serial);
    snprintf(s_slots[i].info.host,   sizeof s_slots[i].info.host,   "%s", host);
    snprintf(s_slots[i].info.model,  sizeof s_slots[i].info.model,  "%s", model);
    snprintf(s_slots[i].info.name,   sizeof s_slots[i].info.name,   "%s", name);
    xSemaphoreGive(s_lock);

    if (is_new)
        ESP_LOGI(TAG, "found %s (%s) at %s [%s]", name[0] ? name : "?", serial, host, model);
}

static void parse_datagram(const char *buf, const char *src_ip)
{
    // Only Bambu SSDP: the device type appears in NT (NOTIFY) or ST (200 OK).
    if (!strcasestr(buf, "bambulab-com")) return;

    char serial[32] = {0}, model[24] = {0}, name[32] = {0}, location[64] = {0};
    if (!header(buf, "USN", serial, sizeof serial)) return;   // no serial → useless
    header(buf, "DevModel.bambu.com", model, sizeof model);
    header(buf, "DevName.bambu.com",  name,  sizeof name);
    header(buf, "Location",           location, sizeof location);

    // Trust the datagram source IP; fall back to a bare-IP Location.
    const char *host = (src_ip && src_ip[0]) ? src_ip : location;
    remember(serial, host, model, name);
}

// --- sockets ----------------------------------------------------------------

static uint32_t sta_if_addr(void)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (n && esp_netif_get_ip_info(n, &ip) == ESP_OK) return ip.ip.addr;
    return 0;
}

static int make_socket(uint16_t port)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) {
        ESP_LOGW(TAG, "bind :%u failed (errno %d)", port, errno);
        close(s);
        return -1;
    }
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    return s;
}

static void join_group(int s, uint32_t ifaddr)
{
    struct ip_mreq m = {0};
    m.imr_multiaddr.s_addr = inet_addr(SSDP_GROUP);
    m.imr_interface.s_addr = ifaddr;
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) < 0)
        ESP_LOGD(TAG, "join failed (errno %d)", errno);
    struct in_addr out = { .s_addr = ifaddr };
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &out, sizeof out);
    uint8_t ttl = 2;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
}

static void send_msearch(int s, uint16_t port)
{
    struct sockaddr_in g = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = inet_addr(SSDP_GROUP),
    };
    sendto(s, MSEARCH, sizeof(MSEARCH) - 1, 0, (struct sockaddr *)&g, sizeof g);
}

// One-shot scan: open a socket, probe + listen for the window, then close + exit.
static void scan_task(void *arg)
{
    (void)arg;
    int fd = make_socket(RX_PORT);
    if (fd >= 0) {
        uint32_t ifaddr = sta_if_addr();
        if (ifaddr) join_group(fd, ifaddr);
        static char rx[1600];
        int64_t end = esp_timer_get_time() + SCAN_WINDOW_US;
        int64_t next_probe = 0;
        while (esp_timer_get_time() < end) {
            int64_t now = esp_timer_get_time();
            if (ifaddr && now >= next_probe) {
                for (int i = 0; i < 2; i++) send_msearch(fd, PROBE_PORTS[i]);
                next_probe = now + PROBE_EVERY_MS * 1000LL;
            }
            fd_set r;
            FD_ZERO(&r);
            FD_SET(fd, &r);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
            if (select(fd + 1, &r, NULL, NULL, &tv) <= 0) continue;
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int got = recvfrom(fd, rx, sizeof(rx) - 1, 0, (struct sockaddr *)&from, &fl);
            if (got <= 0) continue;
            rx[got] = '\0';
            char src[INET_ADDRSTRLEN] = {0};
            inet_ntoa_r(from.sin_addr, src, sizeof src);
            parse_datagram(rx, src);
        }
        close(fd);   // free the socket back to the pool
    }
    s_scanning = false;
    vTaskDelete(NULL);
}

// --- public API -------------------------------------------------------------

esp_err_t dc_bambu_scan_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    if (s_scanning) return ESP_OK;   // one at a time (httpd is single-worker)
    s_scanning = true;
    if (xTaskCreate(scan_task, "bambu_scan", 4096, NULL, 4, NULL) != pdPASS) {
        s_scanning = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "SSDP scan started");
    return ESP_OK;
}

bool dc_bambu_scanning(void) { return s_scanning; }

int dc_bambu_discover_get(dc_bambu_found_t *out, int max)
{
    if (!out || max <= 0 || !s_lock) return 0;
    int64_t now = esp_timer_get_time();
    int cnt = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < DC_BAMBU_DISCOVER_MAX; i++) {
        if (!s_slots[i].used) continue;
        int64_t age = now - s_slots[i].last_seen_us;
        if (age > AGE_OUT_US) { s_slots[i].used = false; continue; }
        if (cnt >= max) continue;
        dc_bambu_found_t item = s_slots[i].info;
        item.age_s = (uint32_t)(age / 1000000LL);
        int j = cnt++;                                   // insert keeping ascending age
        while (j > 0 && out[j - 1].age_s > item.age_s) { out[j] = out[j - 1]; j--; }
        out[j] = item;
    }
    xSemaphoreGive(s_lock);
    return cnt;
}
