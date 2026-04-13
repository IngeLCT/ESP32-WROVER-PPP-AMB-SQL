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
#include "geo_cache.h"
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
#define SAMPLES_PER_SEND_WINDOW 60
#define SNTP_TIME_VALID_EPOCH 1609459200  // 2021-01-01 00:00:00 UTC
#define SNTP_SYNC_TIMEOUT_MS 60000
#define SNTP_SYNC_POLL_MS 500

static void apply_city_to_runtime(const char *city_value) {
    const char *resolved_city = (city_value && city_value[0]) ? city_value : "----";
    strlcpy(g_city, resolved_city, sizeof(g_city));
    sensors_set_city_state(g_city);
}

static void apply_cached_city_or_default(const geo_cache_state_t *geo_state,
                                         const char *reason) {
    const char *cached_city = (geo_state && geo_state->last_city[0]) ?
                              geo_state->last_city : "----";
    ESP_LOGI(TAG_APP, "%s. Usando ciudad cacheada: %s", reason, cached_city);
    apply_city_to_runtime(cached_city);
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
static bool system_time_is_valid(void) {
    time_t now = 0;
    time(&now);
    return now > SNTP_TIME_VALID_EPOCH;
}

static bool init_sntp_and_time(void) {
    ESP_LOGI(TAG_APP, "Iniciando SNTP");

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_init();

    // Zona horaria GMT-6 (ajusta si usas horario de verano distinto)
    setenv("TZ", "UTC6", 1);
    tzset();

    ESP_LOGI(TAG_APP, "Esperando sincronizacion SNTP hasta %d ms",
             SNTP_SYNC_TIMEOUT_MS);

    const int max_polls = SNTP_SYNC_TIMEOUT_MS / SNTP_SYNC_POLL_MS;
    for (int i = 0; i < max_polls; ++i) {
        if (system_time_is_valid()) {
            time_t now = 0;
            struct tm tm_info;
            char time_str[32];
            time(&now);
            localtime_r(&now, &tm_info);
            strftime(time_str, sizeof(time_str), "%d-%m-%Y %H:%M:%S", &tm_info);
            ESP_LOGI(TAG_APP, "SNTP listo; hora valida obtenida: %s", time_str);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SNTP_SYNC_POLL_MS));
    }

    time_t now = 0;
    time(&now);
    ESP_LOGW(TAG_APP,
             "Timeout SNTP: hora del sistema sigue invalida (epoch=%lld)",
             (long long)now);
    return false;
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

    int sample_slot = 0;
    uint32_t sum_co2 = 0;
    int scd41_ok_count_5m = 0;
    int scd41_err03_count_5m = 0;
    int scd41_zero_count_5m = 0;

    int sen55_valid_count_5m = 0;

    double sum_pm1p0    = 0;
    double sum_pm2p5    = 0;
    double sum_pm4p0    = 0;
    double sum_pm10p0   = 0;
    double sum_voc      = 0;
    double sum_nox      = 0;
    double sum_sen_temp = 0;
    double sum_sen_hum  = 0;
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
            modem_ppp_force_public_dns();
            ESP_LOGI(TAG_APP, "DNS publicos reaplicados tras reconectar PPP");
            ESP_LOGI(TAG_APP, "PPP reconectado; reanudo medición/envío");
        }

        SensorData data = {0};

        // ----------------- SCD41 -----------------
        esp_err_t scd_ret = sensors_read_scd41(&data);
        int scd_diag = sensors_get_last_scd41_diag();

        if (scd_ret == ESP_OK) {
            sum_co2 += data.co2;
            scd41_ok_count_5m++;
        }

        if (scd_diag == SENSOR_DIAG_OUT_OF_RANGE) {
            scd41_err03_count_5m++;
        }

        if (data.co2 == 0) {
            scd41_zero_count_5m++;
        }

        // ----------------- SEN55 -----------------
        esp_err_t sen_ret = sensors_read_sen55(&data);
        int sen_diag = sensors_get_last_sen55_diag();

        if (sen_ret == ESP_OK) {
            sum_pm1p0    += data.pm1p0;
            sum_pm2p5    += data.pm2p5;
            sum_pm4p0    += data.pm4p0;
            sum_pm10p0   += data.pm10p0;
            sum_voc      += data.voc;
            sum_nox      += data.nox;
            sum_sen_temp += data.sen_temp;
            sum_sen_hum  += data.sen_hum;
            sum_avg_temp += data.avg_temp;
            sum_avg_hum  += data.avg_hum;
            sen55_valid_count_5m++;
        }

    #if LOG_EACH_SAMPLE
        ESP_LOGI(TAG_APP,
            "Muestra %d/%d de 5m | SCD41: co2_raw=%u diag=%02d ret=%s | SEN55: diag=%02d ret=%s",
            sample_slot + 1,
            SAMPLES_PER_SEND_WINDOW,
            data.co2,
            scd_diag,
            esp_err_to_name(scd_ret),
            sen_diag,
            esp_err_to_name(sen_ret));
    #endif

        sample_slot++;

        if (sample_slot >= SAMPLES_PER_SEND_WINDOW) {
            SensorData window_avg = (SensorData){0};

            if (scd41_ok_count_5m > 0) {
                window_avg.co2 = (uint16_t)(sum_co2 / scd41_ok_count_5m);
            }

            if (sen55_valid_count_5m > 0) {
                double denom = (double)sen55_valid_count_5m;
                window_avg.pm1p0    = (float)(sum_pm1p0    / denom);
                window_avg.pm2p5    = (float)(sum_pm2p5    / denom);
                window_avg.pm4p0    = (float)(sum_pm4p0    / denom);
                window_avg.pm10p0   = (float)(sum_pm10p0   / denom);
                window_avg.voc      = (float)(sum_voc      / denom);
                window_avg.nox      = (float)(sum_nox      / denom);
                window_avg.sen_temp = (float)(sum_sen_temp / denom);
                window_avg.sen_hum  = (float)(sum_sen_hum  / denom);
                window_avg.avg_temp = (float)(sum_avg_temp / denom);
                window_avg.avg_hum  = (float)(sum_avg_hum  / denom);
                window_avg.scd_temp = window_avg.avg_temp;
                window_avg.scd_hum  = window_avg.avg_hum;
            }

            ESP_LOGI(TAG_APP,
                     "Resumen 5m | co2=%u scd41_ok=%d scd41_err03=%d scd41_zero=%d sen55_temp_dbg=%.2f sen55_hum_dbg=%.2f",
                     window_avg.co2,
                     scd41_ok_count_5m,
                     scd41_err03_count_5m,
                     scd41_zero_count_5m,
                     window_avg.sen_temp,
                     window_avg.sen_hum);

            time_t now_epoch;
            struct tm tm_info;
            time(&now_epoch);
            localtime_r(&now_epoch, &tm_info);

            char hora_envio[16];
            strftime(hora_envio, sizeof(hora_envio), "%H:%M:%S", &tm_info);

            char fecha_actual[20];
            strftime(fecha_actual, sizeof(fecha_actual), "%d-%m-%Y", &tm_info);

            char json[896];

            bool include_fecha = first_send ||
                                (strncmp(last_fecha_str, fecha_actual,
                                         sizeof(last_fecha_str)) != 0);
            bool day_changed = (!first_send && include_fecha);

            if (day_changed) {
                esp_err_t geo_reset_err = geo_cache_reset_daily_flags();
                if (geo_reset_err == ESP_OK) {
                    ESP_LOGI(TAG_APP,
                             "Cambio de dia detectado (%s). Reset de flags diarios de geolocalizacion",
                             fecha_actual);
                } else {
                    ESP_LOGW(TAG_APP,
                             "Cambio de dia detectado (%s), pero no se pudieron resetear flags geo: %s",
                             fecha_actual,
                             esp_err_to_name(geo_reset_err));
                }
            }

            if (first_send) {
                snprintf(json, sizeof(json),
                    "{\"pm1p0\":%.2f,\"pm2p5\":%.2f,\"pm4p0\":%.2f,\"pm10p0\":%.2f,"
                    "\"voc\":%.1f,\"nox\":%.1f,\"cTe\":%.2f,\"cHu\":%.2f,\"co2\":%u,"
                    "\"sen55_temp_dbg\":%.2f,\"sen55_hum_dbg\":%.2f,"
                    "\"scd41_ok_count_5m\":%d,\"scd41_err03_count_5m\":%d,\"scd41_zero_count_5m\":%d,"
                    "\"fecha\":\"%s\",\"inicio\":\"%s\",\"ciudad\":\"%s\",\"hora\":\"%s\","
                    "\"device_id\":\"%s\"}",
                    window_avg.pm1p0, window_avg.pm2p5, window_avg.pm4p0, window_avg.pm10p0,
                    window_avg.voc, window_avg.nox, window_avg.avg_temp, window_avg.avg_hum, window_avg.co2,
                    window_avg.sen_temp, window_avg.sen_hum,
                    scd41_ok_count_5m, scd41_err03_count_5m, scd41_zero_count_5m,
                    fecha_actual, inicio_str, g_city, hora_envio, DEVICE_ID);

            } else if (include_fecha) {
                snprintf(json, sizeof(json),
                    "{\"pm1p0\":%.2f,\"pm2p5\":%.2f,\"pm4p0\":%.2f,\"pm10p0\":%.2f,"
                    "\"voc\":%.1f,\"nox\":%.1f,\"cTe\":%.2f,\"cHu\":%.2f,\"co2\":%u,"
                    "\"sen55_temp_dbg\":%.2f,\"sen55_hum_dbg\":%.2f,"
                    "\"scd41_ok_count_5m\":%d,\"scd41_err03_count_5m\":%d,\"scd41_zero_count_5m\":%d,"
                    "\"fecha\":\"%s\",\"hora\":\"%s\"}",
                    window_avg.pm1p0, window_avg.pm2p5, window_avg.pm4p0, window_avg.pm10p0,
                    window_avg.voc, window_avg.nox, window_avg.avg_temp, window_avg.avg_hum, window_avg.co2,
                    window_avg.sen_temp, window_avg.sen_hum,
                    scd41_ok_count_5m, scd41_err03_count_5m, scd41_zero_count_5m,
                    fecha_actual, hora_envio);

            } else {
                snprintf(json, sizeof(json),
                    "{\"pm1p0\":%.2f,\"pm2p5\":%.2f,\"pm4p0\":%.2f,\"pm10p0\":%.2f,"
                    "\"voc\":%.1f,\"nox\":%.1f,\"cTe\":%.2f,\"cHu\":%.2f,\"co2\":%u,"
                    "\"sen55_temp_dbg\":%.2f,\"sen55_hum_dbg\":%.2f,"
                    "\"scd41_ok_count_5m\":%d,\"scd41_err03_count_5m\":%d,\"scd41_zero_count_5m\":%d,"
                    "\"hora\":\"%s\"}",
                    window_avg.pm1p0, window_avg.pm2p5, window_avg.pm4p0, window_avg.pm10p0,
                    window_avg.voc, window_avg.nox, window_avg.avg_temp, window_avg.avg_hum, window_avg.co2,
                    window_avg.sen_temp, window_avg.sen_hum,
                    scd41_ok_count_5m, scd41_err03_count_5m, scd41_zero_count_5m,
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

            sample_slot = 0;
            sum_co2 = 0;
            scd41_ok_count_5m = 0;
            scd41_err03_count_5m = 0;
            scd41_zero_count_5m = 0;
            sen55_valid_count_5m = 0;
            sum_pm1p0 = sum_pm2p5 = sum_pm4p0 = sum_pm10p0 = 0;
            sum_voc   = sum_nox   = sum_sen_temp = sum_sen_hum = 0;
            sum_avg_temp = sum_avg_hum = 0;
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

    // === 2) DNS publicos con PPP activo ===
    modem_ppp_force_public_dns();
    ESP_LOGI(TAG_APP, "DNS publicos aplicados tras PPP");
    (void)modem_ppp_dns_probe_many();

    // === 3) SNTP con PPP activo ===
    bool sntp_time_valid = init_sntp_and_time();

    geo_cache_state_t geo_state = {0};
    esp_err_t geo_cache_err = geo_cache_load(&geo_state);
    if (geo_cache_err != ESP_OK) {
        ESP_LOGW(TAG_APP, "No se pudo cargar cache geo desde NVS: %s",
                 esp_err_to_name(geo_cache_err));
    }
    ESP_LOGI(TAG_APP,
             "Geo flags NVS | attempt_done_today=%d rate_limited_today=%d last_city='%s'",
             geo_state.geo_attempt_done_today,
             geo_state.geo_rate_limited_today,
             geo_state.last_city[0] ? geo_state.last_city : "");

    // === 4) Geolocalizacion por celda (AT + UnwiredLabs) => g_city sin comas ===
    if (!UNWIREDLABS_TOKEN[0]) {
        ESP_LOGW(TAG_APP, "UNWIREDLABS_TOKEN vacio. No se consultara UnwiredLabs");
        apply_cached_city_or_default(&geo_state, "Geolocalizacion omitida por token vacio");
    } else if (geo_state.geo_rate_limited_today) {
        ESP_LOGW(TAG_APP, "Geolocalizacion bloqueada por flag diario de rate limit");
        apply_cached_city_or_default(&geo_state, "Rate limit ya detectado hoy");
    } else if (geo_state.geo_attempt_done_today) {
        ESP_LOGI(TAG_APP, "Geolocalizacion bloqueada por flag diario: ya hubo intento hoy");
        apply_cached_city_or_default(&geo_state, "Intento diario ya realizado");
    } else {
        char city[64]  = "";
        char state[64] = "";
        bool rate_limited = false;

        ESP_LOGI(TAG_APP, "Intento unico de geolocalizacion del dia");
        esp_err_t geo_err = modem_unwiredlabs_city_state_once(city, sizeof(city),
                                                              state, sizeof(state),
                                                              &rate_limited);
        if (geo_err == ESP_OK) {
            build_city_hyphen(g_city, sizeof(g_city), city, state); // "Ciudad-Estado"
            ESP_LOGI(TAG_APP, "Geolocalizacion exitosa. Ciudad para JSON: %s", g_city);
            apply_city_to_runtime(g_city);

            esp_err_t save_city_err = geo_cache_store_last_city(g_city);
            if (save_city_err != ESP_OK) {
                ESP_LOGW(TAG_APP, "No se pudo guardar last_city en NVS: %s",
                         esp_err_to_name(save_city_err));
            }

            esp_err_t save_flags_err = geo_cache_set_daily_flags(true, false);
            if (save_flags_err != ESP_OK) {
                ESP_LOGW(TAG_APP, "No se pudieron guardar flags geo de exito: %s",
                         esp_err_to_name(save_flags_err));
            }
        } else if (rate_limited) {
            ESP_LOGW(TAG_APP, "Rate limit detectado en UnwiredLabs. No se volvera a intentar hoy");
            esp_err_t save_flags_err = geo_cache_set_daily_flags(true, true);
            if (save_flags_err != ESP_OK) {
                ESP_LOGW(TAG_APP, "No se pudieron guardar flags geo de rate limit: %s",
                         esp_err_to_name(save_flags_err));
            }
            apply_cached_city_or_default(&geo_state, "Rate limit detectado");
        } else {
            ESP_LOGW(TAG_APP,
                     "Fallo geolocalizacion por celda: %s",
                     esp_err_to_name(geo_err));
            esp_err_t save_flags_err = geo_cache_set_daily_flags(true, false);
            if (save_flags_err != ESP_OK) {
                ESP_LOGW(TAG_APP, "No se pudieron guardar flags geo de fallo: %s",
                         esp_err_to_name(save_flags_err));
            }
            apply_cached_city_or_default(&geo_state, "Fallo de geolocalizacion del dia");
        }
    }

    vTaskDelay(pdMS_TO_TICKS(1500));

    // === 5) Verificacion de actualizacion de firmware ===
    if (sntp_time_valid) {
        ESP_LOGI(TAG_APP, "Hora valida; revisando OTA por HTTPS");
        ota_check_and_update_if_needed();
    } else {
        ESP_LOGW(TAG_APP,
                 "OTA omitida en este arranque: hora del sistema no validada");
    }

    // === 6) Sensores y task de envio a Hostinger (ya SIN Firebase) ===
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

