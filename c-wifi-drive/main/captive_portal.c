/* Captive portal for the setup network (WiFi-Drive-XXXX).
 *
 * Based on ESP-IDF's examples/protocols/http_server/captive_portal: a tiny DNS
 * server answers every A query with the access point's address, DHCP option 114
 * names the settings page, and the file server redirects the OS connectivity
 * probes there. Phones and laptops then show their "sign in to network" pop-up.
 * Runs only while the setup network is up; the station side is untouched.
 */

#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "captive_portal.h"

static const char *TAG = "portal";

#define DNS_PORT        53
#define DNS_BUF_LEN     512                 /* classic DNS-over-UDP maximum */
#define DNS_ANSWER_LEN  16                  /* name pointer + type/class/ttl/len + IPv4 */
#define DNS_TTL_SEC     10                  /* short: clients forget the fake answers quickly */
#define DNS_TASK_STACK  3072

static char s_portal_url[40] = "http://192.168.4.1/settings.html";
static esp_ip4_addr_t s_ap_ip;
static volatile bool s_dns_run;
static TaskHandle_t s_dns_task;

/* Build the reply in place: header + the single question + one A answer.
 * Returns the reply length, or 0 to stay silent. */
static int dns_build_reply(uint8_t *buf, int len)
{
    if (len < 12 || (buf[2] & 0x80) || (buf[2] & 0x78)) {
        return 0;                           /* too short, a response, or not a standard query */
    }
    if (buf[4] != 0 || buf[5] != 1) {
        return 0;                           /* only single-question queries */
    }
    int pos = 12;
    while (pos < len && buf[pos] != 0) {
        if (buf[pos] & 0xC0) {
            return 0;                       /* no compression expected in a question */
        }
        pos += buf[pos] + 1;
    }
    pos += 1 + 4;                           /* root label, QTYPE, QCLASS */
    if (pos > len) {
        return 0;
    }
    uint16_t qtype = (buf[pos - 4] << 8) | buf[pos - 3];
    bool answer = (qtype == 1);             /* A; everything else gets an empty NOERROR */

    buf[2] = 0x80 | (buf[2] & 0x01);        /* QR, keep RD */
    buf[3] = 0x80;                          /* RA, RCODE 0 */
    buf[6] = 0;
    buf[7] = answer ? 1 : 0;                /* ANCOUNT */
    memset(&buf[8], 0, 4);                  /* drop authority/additional (EDNS) records */
    if (!answer) {
        return pos;
    }
    uint8_t *a = &buf[pos];
    a[0] = 0xC0; a[1] = 12;                 /* pointer to the question name */
    a[2] = 0; a[3] = 1;                     /* type A */
    a[4] = 0; a[5] = 1;                     /* class IN */
    a[6] = 0; a[7] = 0; a[8] = 0; a[9] = DNS_TTL_SEC;
    a[10] = 0; a[11] = 4;
    memcpy(&a[12], &s_ap_ip.addr, 4);       /* already network order */
    return pos + DNS_ANSWER_LEN;
}

static void dns_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(DNS_BUF_LEN + DNS_ANSWER_LEN, MALLOC_CAP_SPIRAM);
    if (!buf) {
        buf = malloc(DNS_BUF_LEN + DNS_ANSWER_LEN);
    }
    int sock = buf ? socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : -1;
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS server: no %s", buf ? "socket" : "memory");
        goto out;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = s_ap_ip.addr,    /* AP side only */
    };
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct timeval tv = { .tv_sec = 1 };    /* wake up to notice a stop */
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS server: bind failed, errno %d", errno);
        goto out;
    }
    ESP_LOGI(TAG, "DNS server answering everything with " IPSTR, IP2STR(&s_ap_ip));
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, buf, DNS_BUF_LEN, 0, (struct sockaddr *)&from, &from_len);
        if (len <= 0) {
            continue;                       /* timeout */
        }
        int reply = dns_build_reply(buf, len);
        if (reply > 0) {
            sendto(sock, buf, reply, 0, (struct sockaddr *)&from, from_len);
        }
    }
out:
    if (sock >= 0) {
        close(sock);
    }
    free(buf);
    ESP_LOGI(TAG, "DNS server stopped");
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

void captive_portal_start(esp_netif_t *ap_netif)
{
    esp_netif_ip_info_t ip;
    if (!ap_netif || esp_netif_get_ip_info(ap_netif, &ip) != ESP_OK) {
        return;
    }
    s_ap_ip = ip.ip;
    snprintf(s_portal_url, sizeof(s_portal_url), "http://" IPSTR "/settings.html", IP2STR(&ip.ip));
    /* DHCP option 114 (RFC 8910): the pointer is kept, so it must stay valid */
    esp_err_t err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                           s_portal_url, strlen(s_portal_url));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DHCP captive-portal option: %s", esp_err_to_name(err));
    }
    if (s_dns_task) {
        s_dns_run = true;                   /* still running (or winding down): keep it */
        return;
    }
    s_dns_run = true;
    if (xTaskCreate(dns_task, "dns", DNS_TASK_STACK, NULL, 4, &s_dns_task) != pdPASS) {
        s_dns_task = NULL;
        ESP_LOGE(TAG, "DNS server: task not created");
    }
}

void captive_portal_stop(void)
{
    s_dns_run = false;
    for (int i = 0; i < 30 && s_dns_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));     /* the task notices within 1 s */
    }
}

/* True when the request came in on the setup network's address. */
static bool request_on_ap(httpd_req_t *req)
{
    struct sockaddr_storage local;
    socklen_t len = sizeof(local);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &len) != 0) {
        return false;
    }
    uint32_t addr;
    if (local.ss_family == AF_INET) {
        addr = ((struct sockaddr_in *)&local)->sin_addr.s_addr;
    } else if (local.ss_family == AF_INET6) {
        /* IPv4-mapped (::ffff:a.b.c.d) on the dual-stack listening socket */
        memcpy(&addr, &((struct sockaddr_in6 *)&local)->sin6_addr.s6_addr[12], 4);
    } else {
        return false;
    }
    return addr == s_ap_ip.addr;
}

bool captive_portal_redirect(httpd_req_t *req)
{
    static const char *const probes[] = {
        "/generate_204", "/gen_204",                            /* Android, Chrome */
        "/hotspot-detect.html", "/library/test/success.html",   /* Apple */
        "/connecttest.txt", "/ncsi.txt",                        /* Windows */
        "/canonical.html", "/success.txt",                      /* Firefox */
    };
    if (!s_dns_run || !request_on_ap(req)) {
        return false;
    }
    bool redirect = false;
    size_t plen = strcspn(req->uri, "?");
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]) && !redirect; i++) {
        redirect = strlen(probes[i]) == plen && strncasecmp(req->uri, probes[i], plen) == 0;
    }
    if (!redirect) {
        /* any other host name (the DNS server sent it here) also goes to the portal */
        char host[64], self[16];
        snprintf(self, sizeof(self), IPSTR, IP2STR(&s_ap_ip));
        if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_ERR_NOT_FOUND) {
            host[strcspn(host, ":")] = 0;
            size_t hlen = strlen(host);
            bool mdns = hlen > 6 && strcasecmp(host + hlen - 6, ".local") == 0;   /* wifidrive.local */
            redirect = strcasecmp(host, self) != 0 && !mdns;
        }
    }
    if (!redirect) {
        return false;
    }
    ESP_LOGI(TAG, "redirecting %s to the settings page", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_portal_url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* iOS wants a body, a bare redirect is not recognised (see the IDF example) */
    httpd_resp_sendstr(req, "Redirect to the WiFi drive settings");
    return true;
}
