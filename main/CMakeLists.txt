idf_component_register(
    SRCS    "sensors.c" "main.c" "modem_ppp.c" "ota_update.c"
    INCLUDE_DIRS "." 
    REQUIRES 
        esp_hostinger
        app_update
        driver 
        esp_timer 
        esp_http_client
        esp_https_ota
        esp-tls 
        esp_netif 
        mbedtls
        nvs_flash 
        json
        lwip 
        esp_modem 
        esp_wifi
)
