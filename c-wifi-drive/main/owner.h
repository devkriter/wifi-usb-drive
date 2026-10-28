/* Card ownership: the USB host owns the SD card by default; WiFi file operations
 * "borrow" it (host sees "medium not present"), and it is handed back after an
 * idle period. Only this module moves the storage between USB and the application. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "tinyusb_msc.h"

typedef enum {
    OWNER_NO_CARD = 0,  /* storage not attached yet */
    OWNER_USB,          /* exposed to the USB host */
    OWNER_PENDING,      /* a borrow is waiting for USB to go idle */
    OWNER_APP,          /* mounted for the application (WiFi side) */
} owner_state_t;

typedef struct {
    uint32_t usb_idle_ms;        /* USB must be quiet this long before the card is taken */
    uint32_t release_ms;         /* hand back this long after the last app use */
    uint32_t borrow_timeout_ms;  /* stop waiting for USB idle after this and take it anyway */
    bool hard_handoff;           /* re-enumerate USB after handing back */
} owner_config_t;

typedef struct {
    uint32_t borrows;
    uint32_t releases;
    uint32_t forced_takeovers;
    uint32_t usb_reads;
    uint32_t usb_writes;
    uint32_t last_usb_io_ms;     /* esp_timer ms when the host last read or wrote */
    int64_t state_since_us;
} owner_stats_t;

esp_err_t owner_init(const owner_config_t *cfg);
/* Attach the created storage (must currently be mounted to the application). */
esp_err_t owner_attach_storage(tinyusb_msc_storage_handle_t storage);

/* Blocks until the application owns the card. timeout_ms 0 = configured default. */
esp_err_t owner_borrow(uint32_t timeout_ms);
void owner_release(void);

/* Sticky "edit mode": keep the card on the application side until switched off. */
esp_err_t owner_set_sticky(bool on);
bool owner_is_sticky(void);
/* Hand the card back right now (fails with ESP_ERR_INVALID_STATE while a transfer is active). */
esp_err_t owner_give_back_now(void);

owner_state_t owner_get_state(void);
const char *owner_state_str(owner_state_t s);
owner_stats_t owner_get_stats(void);

/* Called from the TinyUSB MSC completion callbacks. */
void owner_note_usb_io(bool is_write);
/* Host did a SCSI eject ("Safely Remove") or a load / a fresh USB attach. */
void owner_note_host_eject(bool ejected);
bool owner_host_ejected(void);

/* Runtime settings (persisted in NVS). */
uint32_t owner_get_release_ms(void);
void owner_set_release_ms(uint32_t ms);
bool owner_get_hard_handoff(void);
void owner_set_hard_handoff(bool on);
