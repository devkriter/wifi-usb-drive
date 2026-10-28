#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "events.h"

#define EV_COUNT 48
#define EV_LEN   96

static struct {
    SemaphoreHandle_t lock;
    char text[EV_COUNT][EV_LEN];
    uint32_t at_ms[EV_COUNT];
    uint32_t seq[EV_COUNT];
    uint32_t next_seq;
    int head;
    int count;
} s;

static void ev_init(void)
{
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
    }
}

void events_log(const char *fmt, ...)
{
    ev_init();
    char line[EV_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    for (char *p = line; *p; p++) {        /* keep the JSON simple */
        if (*p == '"' || *p == '\\') {
            *p = '\'';
        }
    }
    ESP_LOGI("event", "%s", line);
    xSemaphoreTake(s.lock, portMAX_DELAY);
    strlcpy(s.text[s.head], line, EV_LEN);
    s.at_ms[s.head] = (uint32_t)(esp_timer_get_time() / 1000);
    s.seq[s.head] = s.next_seq++;
    s.head = (s.head + 1) % EV_COUNT;
    if (s.count < EV_COUNT) {
        s.count++;
    }
    xSemaphoreGive(s.lock);
}

size_t events_to_json(char *buf, size_t len)
{
    ev_init();
    size_t n = 0;
    n += snprintf(buf + n, len - n, "[");
    xSemaphoreTake(s.lock, portMAX_DELAY);
    int start = (s.head - s.count + EV_COUNT) % EV_COUNT;
    for (int i = 0; i < s.count && n < len - 2; i++) {
        int idx = (start + i) % EV_COUNT;
        n += snprintf(buf + n, len - n, "%s{\"seq\":%lu,\"uptime_s\":%lu,\"event\":\"%s\"}",
                      i ? "," : "", (unsigned long)s.seq[idx], (unsigned long)(s.at_ms[idx] / 1000), s.text[idx]);
    }
    xSemaphoreGive(s.lock);
    if (n > len - 2) {
        n = len - 2;
    }
    n += snprintf(buf + n, len - n, "]");
    return n;
}
