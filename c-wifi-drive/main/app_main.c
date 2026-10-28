/* WiFi USB drive for the Waveshare ESP32-S3-LCD-1.47.
 *
 * Boot: WiFi + web server first (so the status page works even without a card), then the SD
 * card and USB mass storage. The card starts on the application side for a directory scan,
 * is handed to the USB host, and from then on WiFi file operations borrow it (see owner.c).
 *
 * BOOT button: short press = toggle sticky edit mode (card stays on the WiFi side);
 *              hold 3 s   = restart into the ROM download mode for flashing.
 * LED: green = USB owns the card (flashes on host reads, orange on host writes),
 *      blue = WiFi side owns it, amber = waiting for USB to go idle, red = no card.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "soc/rtc_cntl_reg.h"
#include "led_strip.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "storage.h"
#include "owner.h"
#include "cdc_log.h"
#include "ui.h"
#include "events.h"

static const char *TAG = "main";

#ifdef CONFIG_WD_HARD_HANDOFF
#define WD_HARD_HANDOFF_DEFAULT true
#else
#define WD_HARD_HANDOFF_DEFAULT false
#endif

extern void app_wifi_main(void);
extern esp_err_t start_file_server(const char *base_path);
extern void file_server_prime_cache(const char *dirpath);

static led_strip_handle_t s_led;

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_led) {
        led_strip_set_pixel(s_led, 0, r, g, b);
        led_strip_refresh(s_led);
    }
}

static void led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_WD_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led) != ESP_OK) {
        ESP_LOGW(TAG, "LED init failed");
        s_led = NULL;
    }
}

static void led_task(void *arg)
{
    uint32_t tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        tick++;
        owner_state_t st = owner_get_state();
        owner_stats_t stats = owner_get_stats();
        uint32_t since_io = (uint32_t)(esp_timer_get_time() / 1000) - stats.last_usb_io_ms;
        switch (st) {
        case OWNER_NO_CARD:
            led_set((tick & 8) ? 40 : 0, 0, 0);           /* red blink */
            break;
        case OWNER_USB:
            if (stats.last_usb_io_ms && since_io < 150) {
                led_set(0, 60, 0);                            /* host activity */
            } else {
                led_set(0, 6, 0);                             /* idle, USB owns */
            }
            break;
        case OWNER_PENDING:
            led_set(40, 20, 0);                               /* amber */
            break;
        case OWNER_APP:
            led_set(0, 0, owner_is_sticky() ? 50 : 25);       /* blue */
            break;
        }
    }
}

static void enter_download_mode(void)
{
    ESP_LOGW(TAG, "BOOT held: restarting into ROM download mode");
    events_log("button: download mode");
    /* give Windows a second to dismount the volume cleanly before the device vanishes */
    owner_set_sticky(true);
    vTaskDelay(pdMS_TO_TICKS(1000));
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_WD_BOOT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int held_ms = 0;
    bool long_done = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (gpio_get_level(CONFIG_WD_BOOT_BTN_GPIO) == 0) {
            held_ms += 20;
            if (held_ms >= 3000 && !long_done) {
                long_done = true;
                enter_download_mode();
            }
        } else {
            if (held_ms >= 60 && held_ms < 1500 && !long_done) {
                bool on = !owner_is_sticky();
                ESP_LOGI(TAG, "button: edit mode %s", on ? "ON" : "OFF");
                events_log("button: edit mode %s", on ? "on" : "off");
                owner_set_sticky(on);
            }
            held_ms = 0;
            long_done = false;
        }
    }
}

/* OTA rollback: a new image that does not reach this point healthy is reverted by the bootloader */
static void health_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(20000));
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "image %s marked valid", running->label);
    }
    vTaskDelete(NULL);
}

static void mdns_start(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed");
        return;
    }
    mdns_hostname_set(CONFIG_WD_MDNS_HOSTNAME);
    mdns_instance_name_set("WiFi Drive");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "reachable as http://%s.local/", CONFIG_WD_MDNS_HOSTNAME);
}

void app_main(void)
{
    led_init();
    led_set(10, 10, 10);

    /* NVS first: the ownership settings and WiFi credentials live there */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    events_log("boot (reset reason %d)", (int)esp_reset_reason());

    owner_config_t ocfg = {
        .usb_idle_ms = CONFIG_WD_USB_IDLE_MS,
        .release_ms = CONFIG_WD_RELEASE_MS,
        .borrow_timeout_ms = CONFIG_WD_BORROW_TIMEOUT_MS,
        .hard_handoff = WD_HARD_HANDOFF_DEFAULT,
    };
    ESP_ERROR_CHECK(owner_init(&ocfg));

    app_wifi_main();          /* NVS, netif, WiFi (station and/or AP) */
    mdns_start();
    /* never let a memory shortage here reboot the board: USB must still come up */
    if (start_file_server(STORAGE_BASE_PATH) != ESP_OK) {
        ESP_LOGE(TAG, "web file server failed to start (out of internal RAM?) - continuing without it");
    }
    xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
    xTaskCreate(led_task, "led", 2048, NULL, 3, NULL);

    ESP_ERROR_CHECK(storage_init());                    /* waits for a card; mounted to app */
    ESP_ERROR_CHECK(owner_attach_storage(storage_handle()));
    file_server_prime_cache(STORAGE_BASE_PATH "/");     /* directory listing while USB owns it */
    ESP_ERROR_CHECK(owner_give_back_now());             /* expose to the host */
    ESP_ERROR_CHECK(storage_usb_start());
    cdc_log_init();
    ESP_LOGI(TAG, "ready: card on the USB side, web UI on port 80");
    xTaskCreate(health_task, "health", 3072, NULL, 2, NULL);

    /* the screen comes last so that a display problem can never take USB or WiFi down */
    if (ui_init() != ESP_OK) {
        ESP_LOGW(TAG, "running without the LCD");
    }
}
