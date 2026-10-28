#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "soc/rtc_cntl_reg.h"
#include "owner.h"
#include "storage.h"
#include "dircache.h"
#include "events.h"
#include "api.h"

static esp_err_t events_get_handler(httpd_req_t *req)
{
    char *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    size_t n = events_to_json(buf, 8192);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

static const char *TAG = "api";

extern int app_wifi_get_ip(char *buf, size_t len);
extern int app_wifi_get_rssi(void);
extern bool app_wifi_fallback_ap_active(void);
extern const char *app_wifi_ap_ssid(void);
extern const char *app_wifi_setup_ssid(void);
extern const char *app_wifi_sta_ssid(void);
extern esp_err_t app_wifi_fallback_ap_set(bool on);

/* POST /api/wifi?ap=1|0 : start or stop the setup access point by hand */
static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char query[64], val[8] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
            httpd_query_key_value(query, "ap", val, sizeof(val)) == ESP_OK) {
        esp_err_t err = app_wifi_fallback_ap_set(atoi(val) != 0);
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
            return ESP_FAIL;
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    owner_stats_t st = owner_get_stats();
    uint64_t total = 0, freeb = 0;
    dircache_space(&total, &freeb);
    char ip[32];
    app_wifi_get_ip(ip, sizeof(ip));
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    char elf[9];
    esp_app_get_elf_sha256(elf, sizeof(elf));
    esp_ota_img_states_t img_state = ESP_OTA_IMG_UNDEFINED;
    if (running) {
        esp_ota_get_state_partition(running, &img_state);
    }
    const char *ota_state = (img_state == ESP_OTA_IMG_VALID) ? "valid"
                            : (img_state == ESP_OTA_IMG_PENDING_VERIFY) ? "pending-verify"
                            : (img_state == ESP_OTA_IMG_NEW) ? "new"
                            : (img_state == ESP_OTA_IMG_INVALID) ? "invalid"
                            : (img_state == ESP_OTA_IMG_ABORTED) ? "aborted" : "undefined";
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    char buf[768];
    snprintf(buf, sizeof(buf),
             "{\"owner\":\"%s\",\"sticky\":%s,\"ejected\":%s,\"card\":%s,\"card_name\":\"%s\","
             "\"card_bytes\":%llu,\"total_bytes\":%llu,\"free_bytes\":%llu,\"listing_age_s\":%d,"
             "\"ip\":\"%s\",\"rssi\":%d,\"hostname\":\"%s\",\"sta_ssid\":\"%s\",\"ap_ssid\":\"%s\",\"setup_ssid\":\"%s\",\"fallback_ap\":%s,\"uptime_s\":%lu,"
             "\"usb_reads\":%lu,\"usb_writes\":%lu,\"usb_idle_ms\":%lu,"
             "\"borrows\":%lu,\"releases\":%lu,\"forced\":%lu,"
             "\"release_ms\":%lu,\"hard_handoff\":%s,\"free_heap\":%lu,"
             "\"version\":\"%s\",\"build\":\"%s %s\",\"partition\":\"%s\",\"elf\":\"%s\",\"ota_state\":\"%s\"}",
             owner_state_str(owner_get_state()), owner_is_sticky() ? "true" : "false",
             owner_host_ejected() ? "true" : "false",
             storage_card_present() ? "true" : "false", storage_card_name(),
             (unsigned long long)storage_capacity_bytes(), (unsigned long long)total,
             (unsigned long long)freeb, dircache_age_s(),
             ip, app_wifi_get_rssi(), CONFIG_WD_MDNS_HOSTNAME, app_wifi_sta_ssid(), app_wifi_ap_ssid(), app_wifi_setup_ssid(),
             app_wifi_fallback_ap_active() ? "true" : "false", (unsigned long)(now_ms / 1000),
             (unsigned long)st.usb_reads, (unsigned long)st.usb_writes,
             (unsigned long)(st.last_usb_io_ms ? now_ms - st.last_usb_io_ms : 0),
             (unsigned long)st.borrows, (unsigned long)st.releases, (unsigned long)st.forced_takeovers,
             (unsigned long)owner_get_release_ms(), owner_get_hard_handoff() ? "true" : "false",
             (unsigned long)esp_get_free_heap_size(),
             app->version, app->date, app->time,
             running ? running->label : "?", elf, ota_state);
    return send_json(req, buf);
}

static bool query_value(httpd_req_t *req, const char *key, char *out, size_t outlen)
{
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, key, out, outlen) == ESP_OK;
}

/* POST /api/owner?mode=app|auto|usb */
static esp_err_t owner_post_handler(httpd_req_t *req)
{
    char mode[16] = "";
    if (!query_value(req, "mode", mode, sizeof(mode))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode=app|auto|usb");
        return ESP_FAIL;
    }
    esp_err_t err = ESP_OK;
    events_log("web: owner mode %s", mode);
    if (strcmp(mode, "app") == 0) {
        err = owner_set_sticky(true);
    } else if (strcmp(mode, "auto") == 0) {
        err = owner_set_sticky(false);
    } else if (strcmp(mode, "usb") == 0) {
        err = owner_give_back_now();
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode=app|auto|usb");
        return ESP_FAIL;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"transfer in progress\"}");
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    return send_json(req, "{\"ok\":true}");
}

/* POST /api/settings?release_s=N&hard=0|1 */
static esp_err_t settings_post_handler(httpd_req_t *req)
{
    char val[16];
    if (query_value(req, "release_s", val, sizeof(val))) {
        owner_set_release_ms((uint32_t)atoi(val) * 1000);
    }
    if (query_value(req, "hard", val, sizeof(val))) {
        owner_set_hard_handoff(atoi(val) != 0);
    }
    ESP_LOGI(TAG, "settings: release %lu ms, hard handoff %s",
             (unsigned long)owner_get_release_ms(), owner_get_hard_handoff() ? "on" : "off");
    return send_json(req, "{\"ok\":true}");
}

/* POST /ota with the app .bin as the raw body */
static esp_err_t ota_post_handler(httpd_req_t *req)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }
    if (req->content_len == 0 || req->content_len > next->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad image size");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: %d bytes -> %s", req->content_len, next->label);

    const size_t bufsize = 16 * 1024;
    char *buf = heap_caps_malloc(bufsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    esp_ota_handle_t ota = 0;
    /* erase the needed range up front: measured 19 s for 1.5 MB vs 34 s with
     * OTA_WITH_SEQUENTIAL_WRITES (block erases beat per-sector erases) */
    esp_err_t err = esp_ota_begin(next, req->content_len, &ota);
    int remaining = req->content_len;
    while (err == ESP_OK && remaining > 0) {
        int n = httpd_req_recv(req, buf, (remaining < (int)bufsize) ? remaining : (int)bufsize);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf, n);
        remaining -= n;
    }
    free(buf);
    if (err == ESP_OK) {
        err = esp_ota_end(ota);
    } else {
        esp_ota_abort(ota);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(next);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "OTA done, restarting");
    events_log("OTA update written to %s, restarting", next->label);
    send_json(req, "{\"ok\":true,\"restarting\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t reboot_get_handler(httpd_req_t *req)
{
    events_log("web: reboot requested");
    send_json(req, "{\"ok\":true,\"restarting\":true}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t bootloader_get_handler(httpd_req_t *req)
{
    send_json(req, "{\"ok\":true,\"download_mode\":true}");
    ESP_LOGW(TAG, "restarting into ROM download mode on request");
    events_log("web: download mode requested");
    owner_set_sticky(true);              /* host sees "no media" first */
    vTaskDelay(pdMS_TO_TICKS(1000));
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
    return ESP_OK;
}

esp_err_t api_register(httpd_handle_t server)
{
    const httpd_uri_t uris[] = {
        { .uri = "/api/status",     .method = HTTP_GET,  .handler = status_get_handler },
        { .uri = "/api/events",     .method = HTTP_GET,  .handler = events_get_handler },
        { .uri = "/api/wifi",       .method = HTTP_POST, .handler = wifi_post_handler },
        { .uri = "/api/owner",      .method = HTTP_POST, .handler = owner_post_handler },
        { .uri = "/api/settings",   .method = HTTP_POST, .handler = settings_post_handler },
        { .uri = "/ota",            .method = HTTP_POST, .handler = ota_post_handler },
        { .uri = "/api/reboot",     .method = HTTP_GET,  .handler = reboot_get_handler },
        { .uri = "/api/bootloader", .method = HTTP_GET,  .handler = bootloader_get_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
