/* Small ring of timestamped firmware events (uploads, deletes, handoffs, ejects, reboots),
 * readable at /api/events, so "who touched the card when" has an answer. */
#pragma once
#include <stddef.h>

void events_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Writes the events as a JSON array into buf; returns the number of bytes written. */
size_t events_to_json(char *buf, size_t len);
