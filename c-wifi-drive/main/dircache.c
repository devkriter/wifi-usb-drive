#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "esp_http_server.h"
#include "owner.h"
#include "storage.h"
#include "dircache.h"

static const char *TAG = "dircache";

#define DC_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_FATFS_MAX_LFN)

typedef struct {
    char name[CONFIG_FATFS_MAX_LFN + 1];
    uint32_t size;
    bool is_dir;
} dir_entry_t;

static struct {
    SemaphoreHandle_t lock;
    char path[DC_PATH_MAX];
    dir_entry_t *entries;
    size_t count;
    size_t cap;
    int64_t scanned_us;
    uint64_t total_bytes;
    uint64_t free_bytes;
} s;

static void dc_init(void)
{
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
    }
}

static void url_encode(char *dst, const char *src, size_t dstsize)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 4 < dstsize; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 15];
        }
    }
    dst[o] = '\0';
}

esp_err_t dircache_scan(const char *dirpath)
{
    dc_init();
    DIR *dir = opendir(dirpath);
    if (!dir) {
        ESP_LOGE(TAG, "opendir %s failed", dirpath);
        return ESP_FAIL;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.count = 0;
    strlcpy(s.path, dirpath, sizeof(s.path));
    char entrypath[DC_PATH_MAX];
    strlcpy(entrypath, dirpath, sizeof(entrypath));
    const size_t dirpath_len = strlen(dirpath);
    struct dirent *entry;
    struct stat st;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR && (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)) {
            continue;
        }
        if (s.count == s.cap) {
            size_t ncap = s.cap ? s.cap * 2 : 64;
            dir_entry_t *n = heap_caps_realloc(s.entries, ncap * sizeof(dir_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!n) {
                ESP_LOGE(TAG, "out of memory at %u entries", (unsigned)s.count);
                break;
            }
            s.entries = n;
            s.cap = ncap;
        }
        dir_entry_t *e = &s.entries[s.count];
        strlcpy(e->name, entry->d_name, sizeof(e->name));
        e->is_dir = (entry->d_type == DT_DIR);
        strlcpy(entrypath + dirpath_len, entry->d_name, sizeof(entrypath) - dirpath_len);
        e->size = (stat(entrypath, &st) == 0) ? (uint32_t)st.st_size : 0;
        s.count++;
    }
    closedir(dir);
    s.scanned_us = esp_timer_get_time();
    xSemaphoreGive(s.lock);
    ESP_LOGI(TAG, "scanned %s: %u entries, %llu MB free", dirpath, (unsigned)s.count,
             (unsigned long long)(s.free_bytes / (1024 * 1024)));
    return ESP_OK;
}

/* Recount free space (a full FAT scan: ~seconds on a big card). Application must own the card. */
void dircache_update_space(void)
{
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(STORAGE_BASE_PATH, &total, &freeb) == ESP_OK) {
        s.total_bytes = total;
        s.free_bytes = freeb;
    }
}

void dircache_rescan_last(void)
{
    if (s.scanned_us) {
        char path[DC_PATH_MAX];
        strlcpy(path, s.path, sizeof(path));
        dircache_scan(path);
    }
}

void dircache_space(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (total_bytes) {
        *total_bytes = s.total_bytes;
    }
    if (free_bytes) {
        *free_bytes = s.free_bytes;
    }
}

int dircache_age_s(void)
{
    if (!s.scanned_us) {
        return -1;
    }
    return (int)((esp_timer_get_time() - s.scanned_us) / 1000000);
}

void file_server_prime_cache(const char *dirpath)
{
    dircache_scan(dirpath);
    dircache_update_space();
}

esp_err_t dircache_render(httpd_req_t *req, const char *dirpath)
{
    dc_init();
    const bool force = (strstr(req->uri, "refresh=1") != NULL);
    const bool cached = (s.scanned_us != 0) && (strcmp(s.path, dirpath) == 0);

    if (force || !cached || owner_get_state() == OWNER_APP) {
        if (owner_borrow(force ? 0 : 5000) == ESP_OK) {
            esp_err_t err = dircache_scan(dirpath);
            if (force) {
                dircache_update_space();
            }
            owner_release();
            if (err != ESP_OK && !cached) {
                httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Directory does not exist");
                return ESP_FAIL;
            }
        } else if (!cached) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_sendstr(req, "The card is busy on the USB side; try again in a moment");
            return ESP_FAIL;
        }
    }

    extern const unsigned char file_list_1_start[] asm("_binary_file_list_1_html_start");
    extern const unsigned char file_list_1_end[]   asm("_binary_file_list_1_html_end");
    extern const unsigned char file_list_2_start[] asm("_binary_file_list_2_html_start");
    extern const unsigned char file_list_2_end[]   asm("_binary_file_list_2_html_end");

    /* link base = request path without the query string */
    char base[DC_PATH_MAX];
    strlcpy(base, req->uri, sizeof(base));
    char *q = strchr(base, '?');
    if (q) {
        *q = '\0';
    }

    httpd_resp_send_chunk(req, (const char *)file_list_1_start, file_list_1_end - file_list_1_start);

    char encoded[CONFIG_FATFS_MAX_LFN * 3 + 1];
    char sizestr[16];
    xSemaphoreTake(s.lock, portMAX_DELAY);
    for (size_t i = 0; i < s.count; i++) {
        const dir_entry_t *e = &s.entries[i];
        url_encode(encoded, e->name, sizeof(encoded));
        snprintf(sizestr, sizeof(sizestr), "%lu", (unsigned long)e->size);
        httpd_resp_sendstr_chunk(req, "<tr><td class=\"");
        httpd_resp_sendstr_chunk(req, e->is_dir ? "directory" : "file");
        httpd_resp_sendstr_chunk(req, "\"><a href=\"");
        httpd_resp_sendstr_chunk(req, base);
        httpd_resp_sendstr_chunk(req, encoded);
        if (e->is_dir) {
            httpd_resp_sendstr_chunk(req, "/");
        }
        httpd_resp_sendstr_chunk(req, "\">");
        httpd_resp_sendstr_chunk(req, e->name);
        httpd_resp_sendstr_chunk(req, "</a></td><td>");
        /* never send an empty chunk: esp_http_server takes it as end-of-response */
        httpd_resp_sendstr_chunk(req, e->is_dir ? "-" : sizestr);
        httpd_resp_sendstr_chunk(req, "</td><td><button class=\"deleteButton\" filepath=\"");
        httpd_resp_sendstr_chunk(req, base);
        httpd_resp_sendstr_chunk(req, encoded);
        httpd_resp_sendstr_chunk(req, "\">Delete</button></td></tr>\n");
    }
    xSemaphoreGive(s.lock);

    httpd_resp_send_chunk(req, (const char *)file_list_2_start, file_list_2_end - file_list_2_start);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}
