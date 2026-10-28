idf_component_register(
    SRCS "app_main.c"
         "storage.c"
         "owner.c"
         "dircache.c"
         "api.c"
         "cdc_log.c"
         "events.c"
         "ui.c"
         "app_wifi.c"
         "app_http_server.c"
         "captive_portal.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES fatfs sdmmc esp_driver_sdmmc esp_driver_gpio esp_driver_spi esp_driver_ledc esp_lcd
                  esp_wifi esp_event esp_netif lwip nvs_flash esp_http_server app_update esp_app_format
                  esp_partition esp_timer esp_ringbuf vfs
    EMBED_FILES "../web/favicon.ico"
                "../web/upload.html"
                "../web/file_list_1.html"
                "../web/file_list_2.html"
                "../web/settings.html"
                "../web/styles.css"
)
