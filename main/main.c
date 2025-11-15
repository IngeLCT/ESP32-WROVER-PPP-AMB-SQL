// Core
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <string.h>

// FreeRTOS / ESP-IDF
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"

// Project
#include "sensors.h"
#include "hostinger_ingest.h"
#include "Privado.h"

// PPP / Módem
#include "modem_ppp.h"
#include "esp_modem_api.h"

// Solo para apagar WiFi duro (liberar netifs, etc.)
#include "esp_wifi.h"

static const char *TAG_APP = "app";
static esp_modem_dce_t *g_dce = NULL;

// ---- Ciudad global para el JSON ----
static char g_city[64]  = "----";

#define LOG_EACH_SAMPLE 1
#define SENSOR_TASK_STACK 10240

// Opcional: helper (por ahora no lo usamos, pero no estorba)
static inline int64_t minutes_to_us(int m) { return (int64_t)m * 60 * 1000000; }

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
    SensorData data;

    // Hora de arranque (inicio) para JSON de primer envío
    time_t start_epoch;
    struct tm start_tm_info;
    char inicio_str[20];
    time(&start_epoch);
    localtime_r(&start_epoch, &start_tm_info);
    strftime(inicio_str, sizeof(inicio_str), "%H:%M:%S", &start_tm_info);

    bool first_send = true;

    vTaskDelay(pdMS_TO_TICKS(1000));

    // "Delete on boot" ahora contra Hostinger / SQL
    hostinger_delete_all_for_device(DEVICE_ID);

    // 1 muestra/minuto, envío cada 5 min (igual que el WiFi)
    const int SAMPLE_EVERY_MIN   = 1;
    const int SAMPLES_PER_BATCH  = 5;
    const TickType_t SAMPLE_DELAY_TICKS =
        pdMS_TO_TICKS(SAMPLE_EVERY_MIN * 60000);

    int sample_count = 0;

    double   sum_pm1p0    = 0;
    double   sum_pm2p5    = 0;
    double   sum_pm4p0    = 0;
    double   sum_pm10p0   = 0;
    double   sum_voc      = 0;
    double   sum_nox      = 0;
    double   sum_avg_temp = 0;
    double   sum_avg_hum  = 0;
    uint32_t sum_co2      = 0;

    char last_fecha_str[20] = "";

    // Retención aproximada: usaremos contadores como en el WiFi
    const size_t MAX_BYTES = 10 * 1024 * 1024;
    static double   avg_size      = 256.0;
    static uint32_t approx_count  = 0;

    while (1) {
        // Lectura de sensores
        if (sensors_read(&data) == ESP_OK) {
            sample_count++;
            sum_pm1p0    += data.pm1p0;
            sum_pm2p5    += data.pm2p5;
            sum_pm4p0    += data.pm4p0;
            sum_pm10p0   += data.pm10p0;
            sum_voc      += data.voc;
            sum_nox      += data.nox;
            sum_avg_temp += data.avg_temp;
            sum_avg_hum  += data.avg_hum;
            sum_co2      += data.co2;

#if LOG_EACH_SAMPLE
            ESP_LOGI(TAG_APP,
                "Muestra %d/%d: PM1.0=%.2f PM2.5=%.2f PM4.0=%.2f PM10=%.2f "
                "VOC=%.1f NOx=%.1f CO2=%u Temp=%.2fC Hum=%.2f%%",
                sample_count, SAMPLES_PER_BATCH,
                data.pm1p0, data.pm2p5, data.pm4p0, data.pm10p0,
                data.voc, data.nox, data.co2, data.avg_temp, data.avg_hum);
#endif
        } else {
            ESP_LOGW(TAG_APP, "Error leyendo sensores (batch %d)", sample_count);
        }

        if (sample_count >= SAMPLES_PER_BATCH) {
            // Timestamp actual
            time_t now_epoch;
            struct tm tm_info;
            time(&now_epoch);
            localtime_r(&now_epoch, &tm_info);

            char hora_envio[16];
            strftime(hora_envio, sizeof(hora_envio), "%H:%M:%S", &tm_info);

            char fecha_actual[20];
            // IMPORTANTE: mismo formato que el WiFi => DD-MM-YYYY
            strftime(fecha_actual, sizeof(fecha_actual), "%d-%m-%Y", &tm_info);

            // Promedios
            SensorData avg = (SensorData){0};
            double denom   = (double)sample_count;

            avg.pm1p0     = (float)(sum_pm1p0    / denom);
            avg.pm2p5     = (float)(sum_pm2p5    / denom);
            avg.pm4p0     = (float)(sum_pm4p0    / denom);
            avg.pm10p0    = (float)(sum_pm10p0   / denom);
            avg.voc       = (float)(sum_voc      / denom);
            avg.nox       = (float)(sum_nox      / denom);
            avg.avg_temp  = (float)(sum_avg_temp / denom);
            avg.avg_hum   = (float)(sum_avg_hum  / denom);
            avg.co2       = (uint16_t)(sum_co2 / sample_count);

            // Copiamos la misma convención que en WiFi:
            avg.scd_temp  = avg.avg_temp;
            avg.scd_hum   = avg.avg_hum;
            avg.sen_temp  = avg.avg_temp;
            avg.sen_hum   = avg.avg_hum;

            char json[384];

            if (first_send) {
                // Primer envío incluye "inicio"
                sensors_format_json(&avg, hora_envio, fecha_actual,
                                    inicio_str, json, sizeof(json));
                strncpy(last_fecha_str, fecha_actual, sizeof(last_fecha_str) - 1);
                last_fecha_str[sizeof(last_fecha_str) - 1] = '\0';
                first_send = false;
            } else {
                // En siguientes envíos: sólo incluimos "fecha" cuando cambie de día
                if (strncmp(last_fecha_str, fecha_actual,
                            sizeof(last_fecha_str)) != 0) {
                    snprintf(json, sizeof(json),
                        "{\"pm1p0\":%.2f,\"pm2p5\":%.2f,\"pm4p0\":%.2f,\"pm10p0\":%.2f,"
                        "\"voc\":%.1f,\"nox\":%.1f,\"cTe\":%.2f,\"cHu\":%.2f,\"co2\":%u,"
                        "\"fecha\":\"%s\",\"hora\":\"%s\"}",
                        avg.pm1p0, avg.pm2p5, avg.pm4p0, avg.pm10p0,
                        avg.voc, avg.nox, avg.avg_temp, avg.avg_hum,
                        avg.co2, fecha_actual, hora_envio);

                    strncpy(last_fecha_str, fecha_actual,
                            sizeof(last_fecha_str) - 1);
                    last_fecha_str[sizeof(last_fecha_str) - 1] = '\0';
                } else {
                    snprintf(json, sizeof(json),
                        "{\"pm1p0\":%.2f,\"pm2p5\":%.2f,\"pm4p0\":%.2f,\"pm10p0\":%.2f,"
                        "\"voc\":%.1f,\"nox\":%.1f,\"cTe\":%.2f,\"cHu\":%.2f,\"co2\":%u,"
                        "\"hora\":\"%s\"}",
                        avg.pm1p0, avg.pm2p5, avg.pm4p0, avg.pm10p0,
                        avg.voc, avg.nox, avg.avg_temp, avg.avg_hum,
                        avg.co2, hora_envio);
                }
            }

            int batch_minutes = SAMPLES_PER_BATCH * SAMPLE_EVERY_MIN;
            ESP_LOGI(TAG_APP, "JSON promedio %dm: %s", batch_minutes, json);

            // --- Envío a Hostinger (igual patrón que ESP32C3 WiFi) ---
            ESP_LOGI(TAG_APP, "SQL ingest (len=%d)", (int)strlen(json));
            hostinger_ingest_post(json);

            // --- Retención aproximada en Hostinger (por tamaño lógico ~10 MB) ---
            size_t item_len = strlen(json);
            avg_size = (avg_size * 0.9) + (0.1 * (double)item_len);
            approx_count++;

            uint32_t max_items  =
                (uint32_t)(MAX_BYTES / (avg_size > 1.0 ? avg_size : 1.0));
            uint32_t high_water = max_items + 50;

            if (approx_count > high_water) {
                int deleted = hostinger_trim_oldest_batch(DEVICE_ID, 50);
                if (deleted > 0) {
                    approx_count =
                        (approx_count > (uint32_t)deleted)
                            ? (approx_count - (uint32_t)deleted)
                            : 0;
                    ESP_LOGI(TAG_APP,
                             "Retención: borrados %d antiguos. approx_count=%u "
                             "max_items=%u avg=%.1fB",
                             deleted, approx_count, max_items, avg_size);
                }
            }

            // Reset de acumuladores SOLO después de enviar
            sample_count = 0;
            sum_pm1p0 = sum_pm2p5 = sum_pm4p0 = sum_pm10p0 =
            sum_voc   = sum_nox   = sum_avg_temp = sum_avg_hum = 0;
            sum_co2   = 0;
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
                     "No se pudo geolocalizar por celda. Ciudad='----'");
            ESP_LOGW(TAG_APP, "Fallo Ubicacion reiniciando esp...");
            esp_restart();
        }
    } else {
        ESP_LOGW(TAG_APP,
                 "UNWIREDLABS_TOKEN vacío. Ciudad quedará '----'");
        sensors_set_city_state("----");
    }

    vTaskDelay(pdMS_TO_TICKS(1500));

    // === 4) Sensores y task de envío a Hostinger (ya SIN Firebase) ===
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
