#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "owner.h"
#include "storage.h"
#include "events.h"

static const char *TAG = "owner";

#define NVS_NS "wifidrive"

static struct {
    owner_config_t cfg;
    tinyusb_msc_storage_handle_t storage;
    SemaphoreHandle_t lock;
    esp_timer_handle_t release_timer;
    owner_state_t state;
    int refcount;
    bool sticky;
    owner_stats_t stats;
} s;

static volatile bool s_host_ejected;   /* host did a SCSI eject; cleared on USB attach/load */

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void set_state(owner_state_t st)
{
    if (s.state != st) {
        ESP_LOGI(TAG, "%s -> %s", owner_state_str(s.state), owner_state_str(st));
        s.state = st;
        s.stats.state_since_us = esp_timer_get_time();
    }
}

/* lock held */
static esp_err_t take_card(void)
{
    esp_err_t err = tinyusb_msc_set_storage_mount_point(s.storage, TINYUSB_MSC_STORAGE_MOUNT_APP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount to app failed: %s", esp_err_to_name(err));
        set_state(OWNER_USB);
        return err;
    }
    s.stats.borrows++;
    set_state(OWNER_APP);
    events_log("card -> wifi side (borrow %lu%s)", (unsigned long)s.stats.borrows, s.sticky ? ", edit mode" : "");
    return ESP_OK;
}

/* lock held */
static esp_err_t give_back_card(void)
{
    esp_err_t err = tinyusb_msc_set_storage_mount_point(s.storage, TINYUSB_MSC_STORAGE_MOUNT_USB);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount to usb failed: %s", esp_err_to_name(err));
        return err;
    }
    s.stats.releases++;
    set_state(OWNER_USB);
    events_log("card -> usb host%s", s.cfg.hard_handoff ? " (re-enumerate)" : "");
    if (s.cfg.hard_handoff) {
        ESP_LOGI(TAG, "hard handoff: re-enumerating USB");
        storage_usb_reenumerate();
    }
    return ESP_OK;
}

static void release_timer_cb(void *arg)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (s.refcount == 0 && !s.sticky && s.state == OWNER_APP) {
        give_back_card();
    }
    xSemaphoreGive(s.lock);
}

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint32_t u32;
    uint8_t u8;
    if (nvs_get_u32(h, "release_ms", &u32) == ESP_OK && u32 >= 500 && u32 <= 600000) {
        s.cfg.release_ms = u32;
    }
    if (nvs_get_u8(h, "hard", &u8) == ESP_OK) {
        s.cfg.hard_handoff = (u8 != 0);
    }
    nvs_close(h);
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        return;
    }
    nvs_set_u32(h, "release_ms", s.cfg.release_ms);
    nvs_set_u8(h, "hard", s.cfg.hard_handoff ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t owner_init(const owner_config_t *cfg)
{
    memset(&s, 0, sizeof(s));
    s.cfg = *cfg;
    s.state = OWNER_NO_CARD;
    s.lock = xSemaphoreCreateMutex();
    if (!s.lock) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t targs = {
        .callback = release_timer_cb,
        .name = "owner_release",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s.release_timer));
    settings_load();
    ESP_LOGI(TAG, "usb idle %u ms, release %u ms, borrow timeout %u ms, hard handoff %s",
             (unsigned)s.cfg.usb_idle_ms, (unsigned)s.cfg.release_ms,
             (unsigned)s.cfg.borrow_timeout_ms, s.cfg.hard_handoff ? "on" : "off");
    return ESP_OK;
}

esp_err_t owner_attach_storage(tinyusb_msc_storage_handle_t storage)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.storage = storage;
    tinyusb_msc_mount_point_t mp = TINYUSB_MSC_STORAGE_MOUNT_APP;
    tinyusb_msc_get_storage_mount_point(storage, &mp);
    set_state(mp == TINYUSB_MSC_STORAGE_MOUNT_APP ? OWNER_APP : OWNER_USB);
    xSemaphoreGive(s.lock);
    return ESP_OK;
}

esp_err_t owner_borrow(uint32_t timeout_ms)
{
    if (!s.storage) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms == 0) {
        timeout_ms = s.cfg.borrow_timeout_ms;
    }
    const uint32_t deadline = now_ms() + timeout_ms;

    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.refcount++;
    esp_timer_stop(s.release_timer);   /* cancel a pending hand-back, if any */

    esp_err_t res = ESP_OK;
    while (s.state != OWNER_APP) {
        if (s.state == OWNER_USB) {
            set_state(OWNER_PENDING);
        }
        uint32_t quiet_ms = now_ms() - s.stats.last_usb_io_ms;
        /* after a host eject the medium is unused: no need to wait for idle */
        if (s.stats.last_usb_io_ms == 0 || quiet_ms >= s.cfg.usb_idle_ms || s_host_ejected) {
            res = take_card();
            break;
        }
        if ((int32_t)(now_ms() - deadline) >= 0) {
#ifdef CONFIG_WD_BORROW_FORCE
            ESP_LOGW(TAG, "USB never went idle for %u ms; taking the card anyway",
                     (unsigned)s.cfg.usb_idle_ms);
            s.stats.forced_takeovers++;
            res = take_card();
#else
            ESP_LOGW(TAG, "USB stayed busy for %u ms; leaving the card with the host",
                     (unsigned)timeout_ms);
            set_state(OWNER_USB);
            res = ESP_ERR_TIMEOUT;
#endif
            break;
        }
        xSemaphoreGive(s.lock);
        vTaskDelay(pdMS_TO_TICKS(100));
        xSemaphoreTake(s.lock, portMAX_DELAY);
    }
    if (res != ESP_OK) {
        s.refcount--;
    }
    xSemaphoreGive(s.lock);
    return res;
}

void owner_release(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (s.refcount > 0) {
        s.refcount--;
    }
    if (s.refcount == 0 && !s.sticky && s.state == OWNER_APP) {
        esp_timer_stop(s.release_timer);
        esp_timer_start_once(s.release_timer, (uint64_t)s.cfg.release_ms * 1000);
    }
    xSemaphoreGive(s.lock);
}

esp_err_t owner_set_sticky(bool on)
{
    if (on) {
        xSemaphoreTake(s.lock, portMAX_DELAY);
        bool was = s.sticky;
        s.sticky = true;
        xSemaphoreGive(s.lock);
        if (was) {
            return ESP_OK;
        }
        esp_err_t err = owner_borrow(0);  /* the sticky mode holds one reference */
        if (err != ESP_OK) {
            xSemaphoreTake(s.lock, portMAX_DELAY);
            s.sticky = false;
            xSemaphoreGive(s.lock);
        }
        return err;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    bool was = s.sticky;
    s.sticky = false;
    xSemaphoreGive(s.lock);
    if (was) {
        owner_release();
    }
    return ESP_OK;
}

bool owner_is_sticky(void)
{
    return s.sticky;
}

esp_err_t owner_give_back_now(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (s.sticky) {
        s.sticky = false;
        if (s.refcount > 0) {
            s.refcount--;
        }
    }
    esp_err_t res = ESP_OK;
    if (s.refcount > 0) {
        res = ESP_ERR_INVALID_STATE;   /* a transfer is still using the card */
    } else if (s.state == OWNER_APP) {
        esp_timer_stop(s.release_timer);
        res = give_back_card();
    }
    xSemaphoreGive(s.lock);
    return res;
}

owner_state_t owner_get_state(void)
{
    return s.state;
}

const char *owner_state_str(owner_state_t st)
{
    switch (st) {
    case OWNER_NO_CARD: return "no-card";
    case OWNER_USB:     return "usb";
    case OWNER_PENDING: return "pending";
    case OWNER_APP:     return "app";
    default:            return "?";
    }
}

owner_stats_t owner_get_stats(void)
{
    return s.stats;
}

void owner_note_usb_io(bool is_write)
{
    s.stats.last_usb_io_ms = now_ms();
    if (is_write) {
        s.stats.usb_writes++;
    } else {
        s.stats.usb_reads++;
    }
}

void owner_note_host_eject(bool ejected)
{
    if (s_host_ejected != ejected) {
        s_host_ejected = ejected;
        ESP_LOGI(TAG, "host %s the medium", ejected ? "ejected" : "(re)attached");
        events_log(ejected ? "host ejected the medium" : "host attached");
    }
}

bool owner_host_ejected(void)
{
    return s_host_ejected;
}

uint32_t owner_get_release_ms(void)
{
    return s.cfg.release_ms;
}

void owner_set_release_ms(uint32_t ms)
{
    if (ms < 500) {
        ms = 500;
    }
    if (ms > 600000) {
        ms = 600000;
    }
    s.cfg.release_ms = ms;
    settings_save();
}

bool owner_get_hard_handoff(void)
{
    return s.cfg.hard_handoff;
}

void owner_set_hard_handoff(bool on)
{
    s.cfg.hard_handoff = on;
    settings_save();
}
