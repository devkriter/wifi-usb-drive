#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tinyusb_cdc_acm.h"
#include "storage.h"
#include "owner.h"

static const char *TAG = "storage";

#ifdef CONFIG_WD_SD_4BIT
#define SD_BUS_WIDTH 4
#else
#define SD_BUS_WIDTH 1
#endif

static sdmmc_card_t *s_card;
static tinyusb_msc_storage_handle_t s_storage;
static bool s_usb_started;

static esp_err_t sd_init_once(sdmmc_card_t *card)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = CONFIG_WD_SD_FREQ_KHZ;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = CONFIG_WD_SD_CLK_PIN;
    slot.cmd = CONFIG_WD_SD_CMD_PIN;
    slot.d0 = CONFIG_WD_SD_D0_PIN;
#if CONFIG_WD_SD_4BIT
    slot.width = 4;
    slot.d1 = CONFIG_WD_SD_D1_PIN;
    slot.d2 = CONFIG_WD_SD_D2_PIN;
    slot.d3 = CONFIG_WD_SD_D3_PIN;
#else
    slot.width = 1;
#endif
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    static bool host_ready;
    if (!host_ready) {
        ESP_RETURN_ON_ERROR(host.init(), TAG, "sdmmc host init");
        ESP_RETURN_ON_ERROR(sdmmc_host_init_slot(host.slot, &slot), TAG, "sdmmc slot init");
        host_ready = true;
    }
    return sdmmc_card_init(&host, card);
}

static void msc_event_cb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_START:
        ESP_LOGD(TAG, "mount start -> %s", event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP ? "app" : "usb");
        break;
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
        ESP_LOGI(TAG, "storage now on the %s side", event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP ? "application" : "USB");
        break;
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
        ESP_LOGE(TAG, "mount failed");
        break;
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
        ESP_LOGE(TAG, "card has no FAT filesystem; format it as FAT32 on a PC (not done automatically)");
        break;
    default:
        break;
    }
}

esp_err_t storage_init(void)
{
    s_card = calloc(1, sizeof(sdmmc_card_t));
    if (!s_card) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "SD: %d-bit, %d kHz, CLK %d CMD %d D0 %d", SD_BUS_WIDTH,
             CONFIG_WD_SD_FREQ_KHZ, CONFIG_WD_SD_CLK_PIN, CONFIG_WD_SD_CMD_PIN, CONFIG_WD_SD_D0_PIN);
    int attempt = 0;
    while (sd_init_once(s_card) != ESP_OK) {
        if ((attempt++ % 10) == 0) {
            ESP_LOGE(TAG, "no SD card answered (attempt %d); insert a FAT32 card", attempt);
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    sdmmc_card_print_info(stdout, s_card);

    /* Install the MSC driver ourselves so esp_tinyusb never moves the storage on its own
     * (otherwise every USB connect/reset would yank the card away from the application). */
    tinyusb_msc_driver_config_t drv = {
        .user_flags.auto_mount_off = 1,
        .callback = msc_event_cb,
        .callback_arg = NULL,
    };
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&drv), TAG, "msc driver");

    tinyusb_msc_storage_config_t cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .fat_fs = {
            .base_path = STORAGE_BASE_PATH,
            .config.max_files = CONFIG_FATFS_MAX_FILES,
            .format_flags = 0,
        },
    };
    cfg.medium.card = s_card;
    ESP_RETURN_ON_ERROR(tinyusb_msc_new_storage_sdmmc(&cfg, &s_storage), TAG, "msc storage");
    ESP_LOGI(TAG, "card %s, %llu MB, mounted at %s", s_card->cid.name,
             (unsigned long long)(storage_capacity_bytes() / (1024 * 1024)), STORAGE_BASE_PATH);
    return ESP_OK;
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        ESP_LOGI(TAG, "USB host attached/configured");
        owner_note_host_eject(false);
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        ESP_LOGI(TAG, "USB host detached");
    }
}

esp_err_t storage_usb_start(void)
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.event_cb = usb_event_cb;
    tusb_cfg.event_arg = NULL;
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "tinyusb install");

    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
    };
    esp_err_t err = tinyusb_cdcacm_init(&acm_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CDC init failed (%s); no USB log port", esp_err_to_name(err));
    }
    s_usb_started = true;
    ESP_LOGI(TAG, "USB started (MSC + CDC)");
    return ESP_OK;
}

tinyusb_msc_storage_handle_t storage_handle(void)
{
    return s_storage;
}

sdmmc_card_t *storage_card(void)
{
    return s_card;
}

uint64_t storage_capacity_bytes(void)
{
    if (!s_card) {
        return 0;
    }
    return (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
}

const char *storage_card_name(void)
{
    return s_card ? s_card->cid.name : "";
}

bool storage_card_present(void)
{
    return s_storage != NULL;
}

/* Drive the OTG VBUS-valid inputs low then high through the GPIO matrix: the controller
 * sees a cable pull and re-plug, and the host re-enumerates the device. */
void storage_usb_reenumerate(void)
{
    if (!s_usb_started) {
        return;
    }
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, USB_OTG_VBUSVALID_IN_IDX, 0);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, USB_SRP_BVALID_IN_IDX, 0);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, USB_SRP_SESSEND_IN_IDX, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, USB_OTG_VBUSVALID_IN_IDX, 0);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, USB_SRP_BVALID_IN_IDX, 0);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, USB_SRP_SESSEND_IN_IDX, 0);
}

/* TinyUSB optional callbacks (not implemented by esp_tinyusb): USB activity tracking. */
void tud_msc_read10_complete_cb(uint8_t lun)
{
    owner_note_usb_io(false);
}

void tud_msc_write10_complete_cb(uint8_t lun)
{
    owner_note_usb_io(true);
}

/* START STOP UNIT with LoEj=1: Start=0 is an eject ("Safely Remove"), Start=1 a load. */
void tud_msc_scsi_complete_cb(uint8_t lun, uint8_t const scsi_cmd[16])
{
    if (scsi_cmd[0] == SCSI_CMD_START_STOP_UNIT) {
        bool start = scsi_cmd[4] & 0x01;
        bool loej = scsi_cmd[4] & 0x02;
        if (loej) {
            owner_note_host_eject(!start);
        }
    }
}
