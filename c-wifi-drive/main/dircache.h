/* Directory listing cache: lets the web UI list files while the USB host owns the card. */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

/* Scan a directory into the cache. The application must own the card. */
esp_err_t dircache_scan(const char *dirpath);
/* Re-scan the directory that was scanned last (no-op if none). Application must own the card. */
void dircache_rescan_last(void);
/* Render the file-list page for dirpath, scanning first when needed/allowed. */
esp_err_t dircache_render(httpd_req_t *req, const char *dirpath);
/* Recount free space (full FAT scan); application must own the card. */
void dircache_update_space(void);
/* Card space as of the last recount. */
void dircache_space(uint64_t *total_bytes, uint64_t *free_bytes);
/* Seconds since the last scan, or -1 if never. */
int dircache_age_s(void);
/* Called once at boot while the application owns the card. */
void file_server_prime_cache(const char *dirpath);
