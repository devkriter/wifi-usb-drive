/* Status screen designed to be read from a couple of metres away: one huge colour-coded
 * word for who owns the card, an activity strip, then the essentials in mid-size type.
 *
 *   USB   green   the PC owns the card (strip flashes green on reads, orange on writes)
 *   WIFI  blue    a WiFi file operation has borrowed the card
 *   EDIT  blue    sticky edit mode
 *   WAIT  amber   a WiFi operation is waiting for USB to go idle
 *   NO SD red     no card
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "owner.h"
#include "storage.h"
#include "dircache.h"
#include "ui.h"

static const char *TAG = "ui";

#define LCD_H_RES 172
#define LCD_V_RES 320
#define LCD_SPI_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

extern int app_wifi_get_ip(char *buf, size_t len);
extern int app_wifi_get_rssi(void);
extern bool app_wifi_fallback_ap_active(void);
extern const char *app_wifi_ap_ssid(void);

LV_FONT_DECLARE(lv_font_montserrat_48);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_montserrat_20);

static struct {
    lv_obj_t *pill;
    lv_obj_t *word;
    lv_obj_t *strip;
    lv_obj_t *ip;
    lv_obj_t *host;
    lv_obj_t *space;
    lv_obj_t *bar;
    lv_obj_t *qr;
    lv_obj_t *qr_caption;
    lv_obj_t *qr_ssid;
    char word_txt[8];
    uint32_t pill_color;
    uint32_t strip_color;
    char ip_txt[24];
    char space_txt[24];
    int bar_pct;
} s;

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = CONFIG_WD_LCD_BL_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = (1023 * CONFIG_WD_LCD_BRIGHTNESS) / 100,
        .hpoint = 0,
    };
    ledc_channel_config(&c);
}

static esp_err_t panel_init(esp_lcd_panel_io_handle_t *io_out, esp_lcd_panel_handle_t *panel_out)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = CONFIG_WD_LCD_SCLK_PIN,
        .mosi_io_num = CONFIG_WD_LCD_MOSI_PIN,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 20 * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = CONFIG_WD_LCD_DC_PIN,
        .cs_gpio_num = CONFIG_WD_LCD_CS_PIN,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &io);
    if (err != ESP_OK) {
        return err;
    }
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CONFIG_WD_LCD_RST_PIN,
#ifdef CONFIG_WD_LCD_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    err = esp_lcd_new_panel_st7789(io, &panel_config, &panel);
    if (err != ESP_OK) {
        return err;
    }
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
#ifdef CONFIG_WD_LCD_INVERT
    esp_lcd_panel_invert_color(panel, true);
#endif
    /* the 172-wide glass sits in the middle of the controller's 240 columns */
    esp_lcd_panel_set_gap(panel, 34, 0);
#ifdef CONFIG_WD_LCD_ROTATE_180
    esp_lcd_panel_mirror(panel, true, true);
#else
    esp_lcd_panel_mirror(panel, false, false);
#endif
    esp_lcd_panel_disp_on_off(panel, true);
    *io_out = io;
    *panel_out = panel;
    return ESP_OK;
}

static lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *make_label(lv_obj_t *parent, int y, const lv_font_t *font, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    lv_obj_set_width(l, LCD_H_RES);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, 0, y);
    return l;
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s.pill = make_box(scr, 8, 12, LCD_H_RES - 16, 104, 20, 0x404040);
    s.word = lv_label_create(s.pill);
    lv_obj_set_style_text_font(s.word, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s.word, lv_color_white(), 0);
    lv_label_set_text(s.word, "BOOT");
    lv_obj_center(s.word);

    s.strip = make_box(scr, 8, 126, LCD_H_RES - 16, 18, 7, 0x202020);

    s.ip = make_label(scr, 162, &lv_font_montserrat_24, 0xFFFFFF, "no network");
    s.host = make_label(scr, 196, &lv_font_montserrat_20, 0x9A9A9A, CONFIG_WD_MDNS_HOSTNAME ".local");
    s.space = make_label(scr, 244, &lv_font_montserrat_24, 0xFFFFFF, "");

    s.bar = lv_bar_create(scr);
    lv_obj_set_size(s.bar, LCD_H_RES - 16, 14);
    lv_obj_set_pos(s.bar, 8, 296);
    lv_bar_set_range(s.bar, 0, 100);
    lv_bar_set_value(s.bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s.bar, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s.bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s.bar, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s.bar, lv_color_hex(0x3A86FF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s.bar, 5, LV_PART_INDICATOR);

    /* QR code to join the setup network; shown instead of the details while that network is up */
    /* 128 px: the default name + password give a version-3 code (29 modules), which lands on
     * 4 px per module with a 6 px quiet zone; the white border keeps a zone when the margin is small */
    s.qr = lv_qrcode_create(scr);
    lv_qrcode_set_size(s.qr, 128);
    lv_qrcode_set_dark_color(s.qr, lv_color_black());
    lv_qrcode_set_light_color(s.qr, lv_color_white());
    lv_obj_set_style_border_color(s.qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s.qr, 4, 0);
    lv_obj_set_pos(s.qr, (LCD_H_RES - 128) / 2, 145);
    lv_obj_add_flag(s.qr, LV_OBJ_FLAG_HIDDEN);
    s.qr_ssid = make_label(scr, 275, &lv_font_montserrat_20, 0xFFD060, "");
    lv_obj_add_flag(s.qr_ssid, LV_OBJ_FLAG_HIDDEN);
    s.qr_caption = make_label(scr, 297, &lv_font_montserrat_20, 0xFFFFFF, "192.168.4.1");
    lv_obj_add_flag(s.qr_caption, LV_OBJ_FLAG_HIDDEN);
}

static void show_setup_qr(bool on)
{
    lv_obj_t *details[] = { s.ip, s.host, s.space, s.bar };
    for (size_t i = 0; i < sizeof(details) / sizeof(details[0]); i++) {
        if (on) {
            lv_obj_add_flag(details[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(details[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (on) {
        char join[96];
        snprintf(join, sizeof(join), "WIFI:T:%s;S:%s;P:%s;;",
                 strlen(CONFIG_WD_FALLBACK_AP_PASSWORD) >= 8 ? "WPA" : "nopass",
                 app_wifi_ap_ssid(), CONFIG_WD_FALLBACK_AP_PASSWORD);
        lv_qrcode_update(s.qr, join, strlen(join));
        lv_label_set_text(s.qr_ssid, app_wifi_ap_ssid());
        lv_obj_remove_flag(s.qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s.qr_ssid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s.qr_caption, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s.qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s.qr_ssid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s.qr_caption, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_pill(const char *txt, uint32_t color)
{
    if (strcmp(s.word_txt, txt) != 0) {
        strlcpy(s.word_txt, txt, sizeof(s.word_txt));
        lv_label_set_text(s.word, txt);
        lv_obj_center(s.word);
    }
    if (s.pill_color != color) {
        s.pill_color = color;
        lv_obj_set_style_bg_color(s.pill, lv_color_hex(color), 0);
    }
}

static void set_strip(uint32_t color)
{
    if (s.strip_color != color) {
        s.strip_color = color;
        lv_obj_set_style_bg_color(s.strip, lv_color_hex(color), 0);
    }
}

static void ui_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!lvgl_port_lock(50)) {
            continue;
        }
        owner_state_t st = owner_get_state();
        owner_stats_t stats = owner_get_stats();
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t since_io = now - stats.last_usb_io_ms;

        switch (st) {
        case OWNER_NO_CARD: set_pill("NO SD", 0xC0202A); break;
        case OWNER_USB:
            if (owner_host_ejected()) {
                set_pill("EJECT", 0x505050);   /* host did "Safely Remove": free for WiFi use */
            } else {
                set_pill("USB", 0x1E8E3E);
            }
            break;
        case OWNER_PENDING: set_pill("WAIT", 0xC98A00); break;
        case OWNER_APP:     set_pill(owner_is_sticky() ? "EDIT" : "WIFI", 0x1E5BD6); break;
        }
        if (st == OWNER_USB && stats.last_usb_io_ms && since_io < 300) {
            /* most recent host activity: reads flash green, writes orange */
            static uint32_t last_reads, last_writes;
            uint32_t color = (stats.usb_writes != last_writes) ? 0xFF8C00 : 0x40E060;
            last_reads = stats.usb_reads;
            last_writes = stats.usb_writes;
            set_strip(color);
        } else if (st == OWNER_APP) {
            set_strip(0x3A86FF);
        } else {
            set_strip(0x202020);
        }

        char ip[24];
        app_wifi_get_ip(ip, sizeof(ip));
        if (strcmp(ip, "-") == 0) {
            strlcpy(ip, "no network", sizeof(ip));
        }
        if (strcmp(s.ip_txt, ip) != 0) {
            strlcpy(s.ip_txt, ip, sizeof(s.ip_txt));
            lv_label_set_text(s.ip, ip);
        }
        /* second line: the setup network's name while it is up, else the mDNS name */
        static bool shown_ap;
        bool ap = app_wifi_fallback_ap_active();
        if (ap != shown_ap) {
            shown_ap = ap;
            lv_label_set_text(s.host, ap ? app_wifi_ap_ssid() : CONFIG_WD_MDNS_HOSTNAME ".local");
            lv_obj_set_style_text_color(s.host, lv_color_hex(ap ? 0xFFD060 : 0x9A9A9A), 0);
            show_setup_qr(ap);   /* phone-scannable join code while the setup network is up */
        }

        uint64_t total = 0, freeb = 0;
        dircache_space(&total, &freeb);
        char space[24];
        int pct = 0;
        if (st == OWNER_NO_CARD || total == 0) {
            strlcpy(space, "no card", sizeof(space));
        } else {
            double gb = (double)freeb / (1024.0 * 1024.0 * 1024.0);
            snprintf(space, sizeof(space), "%.1f GB free", gb);
            pct = (int)(100 - (freeb * 100) / total);
        }
        if (strcmp(s.space_txt, space) != 0) {
            strlcpy(s.space_txt, space, sizeof(s.space_txt));
            lv_label_set_text(s.space, space);
        }
        if (pct != s.bar_pct) {
            s.bar_pct = pct;
            lv_bar_set_value(s.bar, pct, LV_ANIM_OFF);
        }
        lvgl_port_unlock();
    }
}

esp_err_t ui_init(void)
{
    backlight_init();
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    esp_err_t err = panel_init(&io, &panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        return err;
    }
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 12288;   /* LVGL 9 software renderer needs more than the 6 KB default */
    ESP_LOGI(TAG, "panel ready, starting LVGL");
    err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl port init failed: %s", esp_err_to_name(err));
        return err;
    }
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * 20,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_LOGI(TAG, "display %s", disp ? "added" : "FAILED");
    if (!disp) {
        ESP_LOGE(TAG, "lvgl display add failed");
        return ESP_FAIL;
    }
    if (lvgl_port_lock(0)) {
        build_screen();
        lvgl_port_unlock();
    }
    xTaskCreate(ui_task, "ui", 6144, NULL, 3, NULL);
    ESP_LOGI(TAG, "LCD up: %dx%d, rotate180=%d, invert=%d, bgr=%d, brightness %d%%", LCD_H_RES, LCD_V_RES,
#ifdef CONFIG_WD_LCD_ROTATE_180
             1,
#else
             0,
#endif
#ifdef CONFIG_WD_LCD_INVERT
             1,
#else
             0,
#endif
#ifdef CONFIG_WD_LCD_BGR
             1,
#else
             0,
#endif
             CONFIG_WD_LCD_BRIGHTNESS);
    return ESP_OK;
}
