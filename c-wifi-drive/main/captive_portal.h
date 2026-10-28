/* Captive portal for the setup network: DNS catch-all, DHCP option 114 and
 * redirects of the OS connectivity probes to the settings page. */
#pragma once
#include <stdbool.h>
#include "esp_netif.h"
#include "esp_http_server.h"

/* Start answering DNS on the AP's address and announce the portal via DHCP. */
void captive_portal_start(esp_netif_t *ap_netif);
/* Stop the DNS server (waits up to ~3 s for its task to exit). */
void captive_portal_stop(void);
/* Called first by the catch-all GET handler: sends a 302 to the settings page
 * and returns true for probes and foreign host names on the setup network. */
bool captive_portal_redirect(httpd_req_t *req);
