#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "esp_log.h"
#include "tusb.h"
#include "tinyusb_cdc_acm.h"
#include "cdc_log.h"

static int cdc_vprintf(const char *fmt, va_list args)
{
    char buf[384];
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    if (n <= 0) {
        return n;
    }
    size_t len = (n < (int)sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
    fwrite(buf, 1, len, stdout);   /* the normal console (UART0) */
    if (tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0) && tud_cdc_n_connected(0)) {
        tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)buf, len);
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    }
    return n;
}

void cdc_log_init(void)
{
    esp_log_set_vprintf(cdc_vprintf);
}
