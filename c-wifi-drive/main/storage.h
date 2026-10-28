/* SD card + USB mass storage (esp_tinyusb 2.x) for the WiFi drive. */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "sdmmc_cmd.h"
#include "tinyusb_msc.h"

#define STORAGE_BASE_PATH "/sdcard"

/* Initialises the SD card (retries until a card answers) and creates the MSC storage,
 * mounted to the application. The USB stack is not started yet. */
esp_err_t storage_init(void);

/* Starts TinyUSB (MSC + CDC). Call once the storage has been handed to the USB side. */
esp_err_t storage_usb_start(void);

tinyusb_msc_storage_handle_t storage_handle(void);
sdmmc_card_t *storage_card(void);
uint64_t storage_capacity_bytes(void);
const char *storage_card_name(void);
bool storage_card_present(void);

/* Fakes a VBUS drop so the host re-enumerates the USB device ("hard handoff"). */
void storage_usb_reenumerate(void);
