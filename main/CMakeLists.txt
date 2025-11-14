idf_component_register(SRCS "sensors.c" "modem_ppp.c" "main.c"
                    INCLUDE_DIRS "." 
                    REQUIRES driver esp_timer esp_http_client esp-tls esp_netif nvs_flash json esp_firebase lwip esp_modem esp_wifi)

