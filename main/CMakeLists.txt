idf_component_register(
    SRCS    "sensors.c" "main.c" "modem_ppp.c" 
    INCLUDE_DIRS "." 
    REQUIRES 
        esp_hostinger
        driver 
        esp_timer 
        esp_http_client
        esp-tls 
        esp_netif 
        nvs_flash 
        json
        lwip 
        esp_modem 
        esp_wifi
)

