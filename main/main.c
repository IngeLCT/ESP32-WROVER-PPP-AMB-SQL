// Core
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <string.h>

// ESP-IDF
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"

// Project
#include "Privado.h"
#include "sensors.h"
#include "hostinger_ingest.h"
#include "ota_update.h"

// PPP / Módem
#include "modem_ppp.h"
#include "esp_modem_api.h"

// Solo para apagar WiFi duro (liberar netifs, etc.)
#include "esp_wifi.h"
#include "esp_app_desc.h"

static const char *TAG_APP = "app";
static esp_modem_dce_t *g_dce = NULL;

// ---- Ciudad global para el JSON ----
static char g_city[64]  = "----";

#define LOG_EACH_SAMPLE        1
#define SENSOR_TASK_STACK      10240

// Reintentos PPP (similar a WiFi)
#define PPP_RECONNECT_WINDOW_MS  60000   // Intentar reconectar hasta 60 s
#define PPP_BACKOFF_IDLE_MS      30000   // Si falla, esperar 30 s y volver a intentar
#define HOSTINGER_POST_MAX_RETRIES 3
#define HOSTINGER_POST_RETRY_DELAY_MS 2000
#define SAMPLE_DELAY_MS 5000
#define SAMPLES_PER_MINUTE 12

static void build_csv_u16(char *dst, size_t dstlen, const uint16_t *values, size_t count) {
    if (!dst || dstlen == 0) return;

    dst[0] = '\0';
    size_t off = 0;
    for (size_t i = 0; i < count && off < dstlen; ++i) {
        int written = snprintf(dst + off,
                               dstlen - off,
                               (i == 0) ? "%u" : ",%u",
                               values[i]);
        if (written < 0) {
            dst[0] = '\0';
            return;
        }
        if ((size_t)written >= (dstlen - off)) {
            dst[dstlen - 1] = '\0';
            return;
        }
        off += (size_t)written;
    }
}

static void build_csv_diag(char *dst, size_t dstlen, const int *values, size_t count) {
    if (!dst || dstlen == 0) return;

    dst[0] = '\0';
    size_t off = 0;
    for (size_t i = 0; i < count && off < dstlen; ++i) {
        int written = snprintf(dst + off,
                               dstlen - off,
                               (i == 0) ? "%02d" : ",%02d",
                               values[i]);
        if (written < 0) {
            dst[0] = '\0';
            return;
        }
        if ((size_t)written >= (dstlen - off)) {
            dst[dstlen - 1] = '\0';
            return;
        }
        off += (size_t)written;
    }
}

// ----------------- WiFi hard off (libera netifs, etc.) -----------------
static void wifi_hard_off(void) {
    // Ignora errores si no estaba inicializado
    esp_wifi_stop();
    esp_wifi_deinit();

    // Borra netifs por si algún código los creó
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) esp_netif_destroy(sta);
    esp_netif_t *ap  = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap)  esp_netif_destroy(ap);
}

// ----------------- SNTP / Hora -----------------
static void init_sntp_and_time(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_init();

    // Zona horaria GMT-6 (ajusta si usas horario de verano distinto)
    setenv("TZ", "UTC6", 1);
    tzset();
    

    // Espera a tener una fecha razonable (> 2021-01-01)
    for (int i = 0; i < 100; ++i) {
        time_t now = 0;
        time(&now);
        if (now > 1609459200) break; // ~2021-01-01
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* Construye "Ciudad-Estado" sin comas (para CSV / Hostinger), con saneo básico */
static void build_city_hyphen(char *dst, size_t dstlen, const char *city, const char *state) {
    if (!dst || dstlen == 0) return;
    const char *c = (city && city[0]) ? city : "----";
    if (state && state[0]) {
        snprintf(dst, dstlen, "%s-%s", c, state);
    } else {
        snprintf(dst, dstlen, "%s", c);
    }
    // Reemplaza cualquier coma/sep raro por '-'
    for (size_t i = 0; dst[i]; ++i) {
        if (dst[i] == ',' || dst[i] == ';' || dst[i] == '|') dst[i] = '-';
    }
}

// ----------------- TASK DE SENSORES + HOSTINGER (PPP) -----------------
static void sensor_task(void *pv) {
    // Hora de arranque (inicio) para JSON de primer envío
    time_t start_epoch;
    struct tm start_tm_info;
    char inicio_str[20];
    time(&start_epoch);
    localtime_r(&start_epoch, &start_tm_info);
    strftime(inicio_str, sizeof(inicio_str), "%H:%M:%S", &start_tm_info);

    bool first_send = true;

    vTaskDelay(pdMS_TO_TICKS(1000));

    // Ya no se realiza borrado al arranque.

    const TickType_t SAMPLE_DELAY_TICKS = pdMS_TO_TICKS(SAMPLE_DELAY_MS);

    int minute_sample_slot = 0;
    uint16_t scd41_co2_raw_1m[SAMPLES_PER_MINUTE] = {0};
    int scd41_diag_raw_1m[SAMPLES_PER_MINUTE] = {0};
    int sen55_diag_raw_1m[SAMPLES_PER_MINUTE] = {0};

    uint32_t sum_co2 = 0;
    int scd41_ok_count_1m = 0;
    int scd41_err03_count_1m = 0;
    int scd41_zero_count_1m = 0;
    uint16_t scd41_co2_last_raw = 0;

    int sen55_valid_count = 0;

    double sum_pm1p0    = 0;
    double sum_pm2p5    = 0;
    double sum_pm4p0    = 0;
    double sum_pm10p0   = 0;
    double sum_voc      = 0;
    double sum_nox      = 0;
    double sum_avg_temp = 0;
    double sum_avg_hum  = 0;

    char last_fecha_str[20] = "";

    while (1) {

        // === Guard de PPP para sensado/envío ===
        if (!modem_ppp_is_connected()) {
            ESP_LOGW(TAG_APP, "PPP caído -> pauso medición/envío y reconecto");
            bool ok = modem_ppp_reconnect_blocking(PPP_RECONNECT_WINDOW_MS);
            if (!ok) {
                ESP_LOGW(TAG_APP,
                        "No se logró reconectar PPP, espero %d ms",
                        PPP_BACKOFF_IDLE_MS);
                vTaskDelay(pdMS_TO_TICKS(PPP_BACKOFF_IDLE_MS));
                continue;
            }
            ESP_LOGI(TAG_APP, "PPP reconectado; reanudo medición/envío");
        }

        SensorData data = {0};

        // ----------------- SCD41 -----------------
        esp_err_t scd_ret = sensors_read_scd41(&data);
        int scd_diag = sensors_get_last_scd41_diag();
        scd41_co2_raw_1m[minute_sample_slot] = data.co2;
        scd41_diag_raw_1m[minute_sample_slot] = scd_diag;
        scd41_co2_last_raw = data.co2;

        if (scd_ret == ESP_OK) {
            sum_co2 += data.co2;
            scd41_ok_count_1m++;
        }

        if (scd_diag == SENSOR_DIAG_OUT_OF_RANGE) {
            scd41_err03_count_1m++;
        }

        if (data.co2 == 0) {
            scd41_zero_count_1m++;
        }

        // ----------------- SEN55 -----------------
        esp_err_t sen_ret = sensors_read_sen55(&data);
        int sen_diag = sensors_get_last_sen55_diag();
        sen55_diag_raw_1m[minute_sample_slot] = sen_diag;

        if (sen_ret == ESP_OK) {
            sum_pm1p0    += data.pm1p0;
            sum_pm2p5    += data.pm2p5;
            sum_pm4p0    += data.pm4p0;
            sum_pm10p0   += data.pm10p0;
            sum_voc      += data.voc;
            sum_nox      += data.nox;
            sum_avg_temp += data.avg_temp;
            sum_avg_hum  += data.avg_hum;
            sen55_valid_count++;
        }

    #if LOG_EACH_SAMPLE
        ESP_LOGI(TAG_APP,
            "Muestra %d/%d | SCD41: co2_raw=%u diag=%02d ret=%s | SEN55: diag=%02d ret=%s",
            minute_sample_slot + 1,
            SAMPLES_PER_MINUTE,
            data.co2,
            scd_diag,
            esp_err_to_name(scd_ret),
            sen_diag,
            esp_err_to_name(sen_ret));
    #endif

        minute_sample_slot++;

        if (minute_sample_slot >= SAMPLES_PER_MINUTE) {
            SensorData minute_avg = (SensorData){0};
            char scd41_co2_raw_csv[96];
            char scd41_diag_raw_csv[48];
            char sen55_diag_raw_csv[48];

            if (scd41_ok_count_1m > 0) {
                minute_avg.co2 = (uint16_t)(sum_co2 / scd41_ok_count_1m);
            }

            if (sen55_valid_count > 0) {
                double denom = (double)sen55_valid_count;
                minute_avg.pm1p0    = (float)(sum_pm1p0    / denom);
                minute_avg.pm2p5    = (float)(sum_pm2p5    / denom);
                minute_avg.pm4p0    = (float)(sum_pm4p0    / denom);
                minute_avg.pm10p0   = (float)(sum_pm10p0   / denom);
                minute_avg.voc      = (float)(sum_voc      / denom);
                minute_avg.nox      = (float)(sum_nox      / denom);
                minute_avg.avg_temp = (float)(sum_avg_temp / denom);
                minute_avg.avg_hum  = (float)(sum_avg_hum  / denom);
                minute_avg.scd_temp = minute_avg.avg_temp;
                minute_avg.scd_hum  = minute_avg.avg_hum;
                minute_avg.sen_temp = minute_avg.avg_temp;
                minute_avg.sen_hum  = minute_avg.avg_hum;
            }

            build_csv_u16(scd41_co2_raw_csv,
                          sizeof(scd41_co2_raw_csv),
                          scd41_co2_raw_1m,
                          SAMPLES_PER_MINUTE);
            build_csv_diag(scd41_diag_raw_csv,
                           sizeof(scd41_diag_raw_csv),
                           scd41_diag_raw_1m,
                           SAMPLES_PER_MINUTE);
            build_csv_diag(sen55_diag_raw_csv,
                           sizeof(sen55_diag_raw_csv),
                           sen55_diag_raw_1m,
                           SAMPLES_PER_MINUTE);

            ESP_LOGI(TAG_APP,
                     "Resumen 1m | co2=%u scd41_ok=%d scd41_err03=%d scd41_zero=%d scd41_last_raw=%u",
                     minute_avg.co2,
                     scd41_ok_count_1m,
                     scd41_err03_count_1m,
                     scd41_zero_count_1m,
                     scd41_co2_last_raw);

            time_t now_epoch;
            struct tm tm_info;
            time(&now_epoch);
            localtime_r(&now_epoch, &tm_info);

            char hora_envio[16];
            strftime(hora_envio, sizeof(hora_envio), "%H:%M:%S", &tm_info);

            char fecha_actual[20];
            strftime(fecha_actual, sizeof(fecha_actual), "%d-%m-%Y", &tm_info);

            char json[1024];

            bool include_fecha = first_send ||
                                (strncmp(last_fecha_str, fecha_actual,
                                         sizeof(last_fecha_str)) != 0);

            if (first_send) {
                snprintf(json, sizeof(json),
                    "{\"pm1p0\":%.2f,\"pm2p5\":%.2f,\"pm4p0\":%.2f,\"pm10p0\":%.2f,"
                    "\"voc\":%.1f,\"nox\":%.1f,\"cTe\":%.2f,\"cHu\":%.2f,\"co2\":%u,"
                    "\"scd41_co2_raw_1m\":\"%s\",\"scd41_diag_raw_1m\":\"%s\",\"sen55_diag_raw_1m\":\"%s\","
                    "\"scd41_ok_count_1m\":%d,\"scd41_err03_count_1m\":%d,\"scd41_zero_count_1m\":%d,"
                    "\"scd41_co2_last_raw\":%u,"
                    "\"fecha\":\"%s\",\"inicio\":\"%s\",\"ciudad\":\"%s\",\"hora\":\"%s\","
                    "\"device_id\":\"%s\"}",
                    minute_avg.pm1p0, minute_avg.pm2p5, minute_avg.pm4p0, minute_avg.pm10p0,
                    minute_avg.voc, minute_avg.nox, minute_avg.avg_temp, minute_avg.avg_hum, minute_avg.co2,
                    scd41_co2_raw_csv, scd41_diag_raw_csv, sen55_diag_raw_csv,
                    scd41_ok_count_1m, scd41_err03_count_1m, scd41_zero_count_1m,
                    scd41_co2_last_raw,
                    fecha_actual, inicio_str, g_city, hora_envio, DEVICE_ID);

            } else if (include_fecha) {
                snprintf(json, sizeof(json),
                    "{\"pm1p0\":%.2f,\"pm2p5\":%.2f,\"pm4p0\":%.2f,\"pm10p0\":%.2f,"
                    "\"voc\":%.1f,\"nox\":%.1f,\"cTe\":%.2f,\"cHu\":%.2f,\"co2\":%u,"
                    "\"scd41_co2_raw_1m\":\"%s\",\"scd41_diag_raw_1m\":\"%s\",\"sen55_diag_raw_1m\":\"%s\","
                    "\"scd41_ok_count_1m\":%d,\"scd41_err03_count_1m\":%d,\"scd41_zero_count_1m\":%d,"
                    "\"scd41_co2_last_raw\":%u,"
                    "\"fecha\":\"%s\",\"hora\":\"%s\"}",
                    minute_avg.pm1p0, minute_avg.pm2p5, minute_avg.pm4p0, minute_avg.pm10p0,
                    minute_avg.voc, minute_avg.nox, minute_avg.avg_temp, minute_avg.avg_hum, minute_avg.co2,
                    scd41_co2_raw_csv, scd41_diag_raw_csv, sen55_diag_raw_csv,
                    scd41_ok_count_1m, scd41_err03_count_1m, scd41_zero_count_1m,
                    scd41_co2_last_raw,
                    fecha_actual, hora_envio);

            } else {
                snprintf(json, sizeof(json),
                    "{\"pm1p0\":%.2f,\"pm2p5\":%.2f,\"pm4p0\":%.2f,\"pm10p0\":%.2f,"
                    "\"voc\":%.1f,\"nox\":%.1f,\"cTe\":%.2f,\"cHu\":%.2f,\"co2\":%u,"
                    "\"scd41_co2_raw_1m\":\"%s\",\"scd41_diag_raw_1m\":\"%s\",\"sen55_diag_raw_1m\":\"%s\","
                    "\"scd41_ok_count_1m\":%d,\"scd41_err03_count_1m\":%d,\"scd41_zero_count_1m\":%d,"
                    "\"scd41_co2_last_raw\":%u,"
                    "\"hora\":\"%s\"}",
                    minute_avg.pm1p0, minute_avg.pm2p5, minute_avg.pm4p0, minute_avg.pm10p0,
                    minute_avg.voc, minute_avg.nox, minute_avg.avg_temp, minute_avg.avg_hum, minute_avg.co2,
                    scd41_co2_raw_csv, scd41_diag_raw_csv, sen55_diag_raw_csv,
                    scd41_ok_count_1m, scd41_err03_count_1m, scd41_zero_count_1m,
                    scd41_co2_last_raw,
                    hora_envio);
            }

    #if LOG_EACH_SAMPLE
            ESP_LOGI(TAG_APP, "JSON promedio/debug: %s", json);
    #endif

            // --- Envío a Hostinger con hasta 3 intentos ---
            int rc = -1;
            for (int attempt = 1; attempt <= HOSTINGER_POST_MAX_RETRIES; ++attempt) {
                rc = hostinger_ingest_post(json);
                if (rc == 0) {
                    if (attempt > 1) {
                        ESP_LOGW(TAG_APP,
                                "Envío a Hostinger exitoso en intento %d/%d",
                                attempt, HOSTINGER_POST_MAX_RETRIES);
                    }
                    break;
                }

                ESP_LOGE(TAG_APP,
                        "Falló envío a Hostinger rc=%d (intento %d/%d)",
                        rc, attempt, HOSTINGER_POST_MAX_RETRIES);

                if (attempt < HOSTINGER_POST_MAX_RETRIES) {
                    vTaskDelay(pdMS_TO_TICKS(HOSTINGER_POST_RETRY_DELAY_MS));
                }
            }

            if (rc != 0) {
                ESP_LOGE(TAG_APP,
                        "Fallaron %d intentos consecutivos a Hostinger. Reiniciando ESP32...",
                        HOSTINGER_POST_MAX_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(HOSTINGER_POST_RETRY_DELAY_MS));
                esp_restart();
            }

            if (include_fecha) {
                strncpy(last_fecha_str, fecha_actual, sizeof(last_fecha_str) - 1);
                last_fecha_str[sizeof(last_fecha_str) - 1] = '\0';
            }
            first_send = false;

            minute_sample_slot = 0;
            memset(scd41_co2_raw_1m, 0, sizeof(scd41_co2_raw_1m));
            memset(scd41_diag_raw_1m, 0, sizeof(scd41_diag_raw_1m));
            memset(sen55_diag_raw_1m, 0, sizeof(sen55_diag_raw_1m));
            sum_co2 = 0;
            scd41_ok_count_1m = 0;
            scd41_err03_count_1m = 0;
            scd41_zero_count_1m = 0;
            scd41_co2_last_raw = 0;
            sen55_valid_count = 0;
            sum_pm1p0 = sum_pm2p5 = sum_pm4p0 = sum_pm10p0 = 0;
            sum_voc   = sum_nox   = sum_avg_temp = sum_avg_hum = 0;
        }

        vTaskDelay(SAMPLE_DELAY_TICKS);
    }
}

// ----------------- APP MAIN (PPP + SNTP + GEO + SENSORES) -----------------
void app_main(void)
{
    // esp_log_level_set("esp_modem", ESP_LOG_VERBOSE);
    // esp_log_level_set("command_lib", ESP_LOG_VERBOSE);

    // NVS básico
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG_APP, "Version local firmware: %s", app_desc->version);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Asegura que no queda nada de WiFi anterior vivo
    wifi_hard_off();

    // === 1) Arranca PPP ===
    modem_ppp_config_t cfg = {
        .tx_io          = 26,
        .rx_io          = 27,
        .rts_io         = -1,
        .cts_io         = -1,    // sin flow control por ahora
        .dtr_io         = 25,
        .rst_io         = 5,
        .pwrkey_io      = 4,
        .board_power_io = 12,
        .rst_active_low = true,
        .rst_pulse_ms   = 200,
        .apn            = "internet.itelcel.com",
        .sim_pin        = "",
        .use_cmux       = false  // ¡dejar en false!
    };

    esp_err_t mret = modem_ppp_start_blocking(&cfg,
                                              150000 /* 150s timeout */,
                                              &g_dce);
    if (mret != ESP_OK) {
        ESP_LOGE(TAG_APP, "No se pudo levantar PPP (%s). Reiniciando...",
                 esp_err_to_name(mret));
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    // === 2) SNTP con PPP activo ===
    init_sntp_and_time();

    // === 3) Geolocalización por celda (AT + UnwiredLabs) => g_city sin comas ===
    if (UNWIREDLABS_TOKEN[0]) {
        char city[64]  = "";
        char state[64] = "";
        if (modem_unwiredlabs_city_state(city, sizeof(city),
                                         state, sizeof(state)) == ESP_OK) {
            build_city_hyphen(g_city, sizeof(g_city), city, state); // "Ciudad-Estado"
            ESP_LOGI(TAG_APP, "Ciudad para JSON (1a vez): %s", g_city);
            sensors_set_city_state(g_city);
        } else {
            ESP_LOGW(TAG_APP,
                     "No se pudo geolocalizar por celda. Usaré ciudad='----'");
            strlcpy(g_city, "----", sizeof(g_city));
            sensors_set_city_state(g_city);
        }
    } else {
        ESP_LOGW(TAG_APP,
                 "UNWIREDLABS_TOKEN vacío. Ciudad quedará 'Morelos'");
        sensors_set_city_state("Morelos");
    }

    vTaskDelay(pdMS_TO_TICKS(1500));

    // === 4) Verificacion  de actulizacion de firmware ===
    ESP_LOGI(TAG_APP, "SNTP listo; revisando OTA por HTTPS");
    ota_check_and_update_if_needed();

    // === 5) Sensores y task de envío a Hostinger (ya SIN Firebase) ===
    esp_err_t sret = sensors_init_all();
    if (sret != ESP_OK) {
        ESP_LOGE(TAG_APP, "Fallo al inicializar sensores: %s",
                 esp_err_to_name(sret));
    } else {
        xTaskCreate(sensor_task,
                    "sensor_task",
                    SENSOR_TASK_STACK,
                    NULL,
                    5,
                    NULL);
    }
}
