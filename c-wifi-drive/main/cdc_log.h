/* Mirrors ESP_LOG output to the USB CDC port, non-blocking (drops when no terminal reads). */
#pragma once
void cdc_log_init(void);
