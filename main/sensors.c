#include "sensors.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "Privado.h"   // DEVICE_ID

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define I2C_MASTER_SCL_IO       19
#define I2C_MASTER_SDA_IO       18
#define I2C_MASTER_FREQ_HZ      100000
#define I2C_PORT                I2C_NUM_0

#define SCD4X_ADDR              0x62
#define SEN5X_ADDR              0x69

static const char *TAG_SENS = "SENSORS";
static char g_city_state[64] = "----";

// ---------- Límites de plausibilidad ----------
#define CO2_MIN_PPM             250
#define CO2_MAX_PPM             10000

#define TEMP_MIN_C              (-20.0f)
#define TEMP_MAX_C              (80.0f)

#define HUM_MIN_PCT             (0.0f)
#define HUM_MAX_PCT             (100.0f)

#define PM_MIN_UGM3             (0.0f)
#define PM_MAX_UGM3             (1000.0f)

#define VOC_MIN_INDEX           (0.0f)
#define VOC_MAX_INDEX           (500.0f)

#define NOX_MIN_INDEX           (0.0f)
#define NOX_MAX_INDEX           (500.0f)

// ---------- Umbrales de salto brusco ----------
#define MAX_STEP_TEMP_C         (5.0f)
#define MAX_STEP_HUM_PCT        (15.0f)
#define MAX_STEP_CO2_PPM        (1200)
#define MAX_STEP_PM_UGM3        (200.0f)
#define MAX_STEP_VOC_INDEX      (120.0f)
#define MAX_STEP_NOX_INDEX      (120.0f)

// ---------- Reintentos / polling ----------
#define SENSOR_READ_ATTEMPTS        2
#define SENSOR_CONFIRM_DELAY_MS     1500
#define SEN5X_READY_POLLS           30
#define SEN5X_READY_POLL_DELAY_MS   20

// ---------- Estado interno ----------
static bool s_has_last_valid = false;
static SensorData s_last_valid = {0};

// ---------- CRC8 ----------
static uint8_t sensirion_crc8(const uint8_t *data, int len) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x31;
            else crc <<= 1;
        }
    }
    return crc;
}

// ---------- I2C v2 handles ----------
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_scd4x_dev = NULL;
static i2c_master_dev_handle_t s_sen5x_dev = NULL;

// ---------- Helpers ----------
static bool is_valid_number(float v) {
    return !isnan(v) && !isinf(v);
}

static bool in_range_f(float v, float vmin, float vmax) {
    return is_valid_number(v) && v >= vmin && v <= vmax;
}

static void set_reason(char *buf, size_t len, const char *fmt, ...) {
    if (!buf || len == 0) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, len, fmt, args);
    va_end(args);
}

static bool validate_ranges(const SensorData *d, char *reason, size_t reason_len) {
    if (!d) {
        set_reason(reason, reason_len, "puntero nulo");
        return false;
    }

    if (d->co2 < CO2_MIN_PPM || d->co2 > CO2_MAX_PPM) {
        set_reason(reason, reason_len, "CO2 fuera de rango: %u", d->co2);
        return false;
    }

    if (!in_range_f(d->scd_temp, TEMP_MIN_C, TEMP_MAX_C)) {
        set_reason(reason, reason_len, "scd_temp fuera de rango: %.2f", d->scd_temp);
        return false;
    }
    if (!in_range_f(d->sen_temp, TEMP_MIN_C, TEMP_MAX_C)) {
        set_reason(reason, reason_len, "sen_temp fuera de rango: %.2f", d->sen_temp);
        return false;
    }
    if (!in_range_f(d->avg_temp, TEMP_MIN_C, TEMP_MAX_C)) {
        set_reason(reason, reason_len, "avg_temp fuera de rango: %.2f", d->avg_temp);
        return false;
    }

    if (!in_range_f(d->scd_hum, HUM_MIN_PCT, HUM_MAX_PCT)) {
        set_reason(reason, reason_len, "scd_hum fuera de rango: %.2f", d->scd_hum);
        return false;
    }
    if (!in_range_f(d->sen_hum, HUM_MIN_PCT, HUM_MAX_PCT)) {
        set_reason(reason, reason_len, "sen_hum fuera de rango: %.2f", d->sen_hum);
        return false;
    }
    if (!in_range_f(d->avg_hum, HUM_MIN_PCT, HUM_MAX_PCT)) {
        set_reason(reason, reason_len, "avg_hum fuera de rango: %.2f", d->avg_hum);
        return false;
    }

    if (!in_range_f(d->pm1p0, PM_MIN_UGM3, PM_MAX_UGM3)) {
        set_reason(reason, reason_len, "pm1p0 fuera de rango: %.2f", d->pm1p0);
        return false;
    }
    if (!in_range_f(d->pm2p5, PM_MIN_UGM3, PM_MAX_UGM3)) {
        set_reason(reason, reason_len, "pm2p5 fuera de rango: %.2f", d->pm2p5);
        return false;
    }
    if (!in_range_f(d->pm4p0, PM_MIN_UGM3, PM_MAX_UGM3)) {
        set_reason(reason, reason_len, "pm4p0 fuera de rango: %.2f", d->pm4p0);
        return false;
    }
    if (!in_range_f(d->pm10p0, PM_MIN_UGM3, PM_MAX_UGM3)) {
        set_reason(reason, reason_len, "pm10p0 fuera de rango: %.2f", d->pm10p0);
        return false;
    }

    if (!in_range_f(d->voc, VOC_MIN_INDEX, VOC_MAX_INDEX)) {
        set_reason(reason, reason_len, "voc fuera de rango: %.2f", d->voc);
        return false;
    }
    if (!in_range_f(d->nox, NOX_MIN_INDEX, NOX_MAX_INDEX)) {
        set_reason(reason, reason_len, "nox fuera de rango: %.2f", d->nox);
        return false;
    }

    return true;
}

static bool validate_step_change(const SensorData *curr,
                                 const SensorData *prev,
                                 char *reason,
                                 size_t reason_len) {
    if (!curr || !prev) return true;

    if (fabsf(curr->avg_temp - prev->avg_temp) > MAX_STEP_TEMP_C) {
        set_reason(reason, reason_len,
                   "salto temp: %.2f -> %.2f", prev->avg_temp, curr->avg_temp);
        return false;
    }

    if (fabsf(curr->avg_hum - prev->avg_hum) > MAX_STEP_HUM_PCT) {
        set_reason(reason, reason_len,
                   "salto hum: %.2f -> %.2f", prev->avg_hum, curr->avg_hum);
        return false;
    }

    if (abs((int)curr->co2 - (int)prev->co2) > MAX_STEP_CO2_PPM) {
        set_reason(reason, reason_len,
                   "salto co2: %u -> %u", prev->co2, curr->co2);
        return false;
    }

    if (fabsf(curr->pm1p0 - prev->pm1p0) > MAX_STEP_PM_UGM3) {
        set_reason(reason, reason_len,
                   "salto pm1p0: %.2f -> %.2f", prev->pm1p0, curr->pm1p0);
        return false;
    }

    if (fabsf(curr->pm2p5 - prev->pm2p5) > MAX_STEP_PM_UGM3) {
        set_reason(reason, reason_len,
                   "salto pm2p5: %.2f -> %.2f", prev->pm2p5, curr->pm2p5);
        return false;
    }

    if (fabsf(curr->pm4p0 - prev->pm4p0) > MAX_STEP_PM_UGM3) {
        set_reason(reason, reason_len,
                   "salto pm4p0: %.2f -> %.2f", prev->pm4p0, curr->pm4p0);
        return false;
    }

    if (fabsf(curr->pm10p0 - prev->pm10p0) > MAX_STEP_PM_UGM3) {
        set_reason(reason, reason_len,
                   "salto pm10p0: %.2f -> %.2f", prev->pm10p0, curr->pm10p0);
        return false;
    }

    if (fabsf(curr->voc - prev->voc) > MAX_STEP_VOC_INDEX) {
        set_reason(reason, reason_len,
                   "salto voc: %.2f -> %.2f", prev->voc, curr->voc);
        return false;
    }

    if (fabsf(curr->nox - prev->nox) > MAX_STEP_NOX_INDEX) {
        set_reason(reason, reason_len,
                   "salto nox: %.2f -> %.2f", prev->nox, curr->nox);
        return false;
    }

    return true;
}

// ---------- SCD4x ----------
static esp_err_t scd4x_start_measurement(void) {
    uint8_t cmd[2] = {0x21, 0xB1};
    return i2c_master_transmit(s_scd4x_dev, cmd, sizeof(cmd), pdMS_TO_TICKS(1000));
}

static esp_err_t scd4x_read_measurement(uint16_t *co2, float *temperature, float *humidity) {
    uint8_t cmd[2] = {0xEC, 0x05};
    esp_err_t ret = i2c_master_transmit(s_scd4x_dev, cmd, sizeof(cmd), pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(1));

    uint8_t data[9];
    ret = i2c_master_receive(s_scd4x_dev, data, sizeof(data), pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) return ret;

    if (sensirion_crc8(&data[0], 2) != data[2] ||
        sensirion_crc8(&data[3], 2) != data[5] ||
        sensirion_crc8(&data[6], 2) != data[8]) {
        ESP_LOGW(TAG_SENS, "SCD4x CRC inválido");
        return ESP_ERR_INVALID_CRC;
    }

    *co2 = ((uint16_t)data[0] << 8) | data[1];
    if (*co2 < CO2_MIN_PPM || *co2 > CO2_MAX_PPM) {
        ESP_LOGW(TAG_SENS, "SCD4x CO2 inválido: %u ppm", *co2);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint16_t raw_temp = ((uint16_t)data[3] << 8) | data[4];
    uint16_t raw_hum  = ((uint16_t)data[6] << 8) | data[7];

    *temperature = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
    *humidity    = 100.0f * ((float)raw_hum / 65535.0f);

    return ESP_OK;
}

// ---------- SEN5x ----------
static esp_err_t sen5x_device_reset(void) {
    uint8_t cmd[2] = {0xD3, 0x04};
    return i2c_master_transmit(s_sen5x_dev, cmd, sizeof(cmd), pdMS_TO_TICKS(1000));
}

static esp_err_t sen5x_start_measurement(void) {
    uint8_t cmd[2] = {0x00, 0x21};
    return i2c_master_transmit(s_sen5x_dev, cmd, sizeof(cmd), pdMS_TO_TICKS(1000));
}

static esp_err_t sen5x_read_data_ready(uint8_t *data_ready) {
    uint8_t cmd[2] = {0x02, 0x02};
    esp_err_t ret = i2c_master_transmit(s_sen5x_dev, cmd, sizeof(cmd), pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t resp[3];
    ret = i2c_master_receive(s_sen5x_dev, resp, sizeof(resp), pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) return ret;

    if (sensirion_crc8(resp, 2) != resp[2]) {
        return ESP_ERR_INVALID_CRC;
    }

    *data_ready = resp[1];
    return ESP_OK;
}

static esp_err_t sen5x_read_measured_values(uint8_t *buf, int buflen) {
    uint8_t cmd[2] = {0x03, 0xC4};
    esp_err_t ret = i2c_master_transmit(s_sen5x_dev, cmd, sizeof(cmd), pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(20));

    return i2c_master_receive(s_sen5x_dev, buf, buflen, pdMS_TO_TICKS(1000));
}

static int sen5x_decode_measurement(const uint8_t *buf,
                                    float *pm1,
                                    float *pm25,
                                    float *pm4,
                                    float *pm10,
                                    float *rh,
                                    float *temp,
                                    float *voc_index,
                                    float *nox_index) {
    uint16_t values[8];

    for (int i = 0; i < 8; i++) {
        const uint8_t *data = &buf[i * 3];
        if (sensirion_crc8(data, 2) != data[2]) return 0;
        values[i] = ((uint16_t)data[0] << 8) | data[1];
    }

    *pm1       = values[0] / 10.0f;
    *pm25      = values[1] / 10.0f;
    *pm4       = values[2] / 10.0f;
    *pm10      = values[3] / 10.0f;
    *rh        = values[4] / 100.0f;
    *temp      = values[5] / 200.0f;
    *voc_index = values[6] / 10.0f;
    *nox_index = values[7] / 10.0f;

    return 1;
}

// ---------- Lectura combinada ----------
static esp_err_t sensors_read_raw(SensorData *out, char *reason, size_t reason_len) {
    if (!out) return ESP_ERR_INVALID_ARG;

    uint16_t co2 = 0;
    float scd_temp = 0.0f, scd_hum = 0.0f;
    uint8_t data_ready = 0;
    uint8_t buf[24];

    esp_err_t ret = scd4x_read_measurement(&co2, &scd_temp, &scd_hum);
    if (ret != ESP_OK) {
        set_reason(reason, reason_len, "falló SCD4x: %s", esp_err_to_name(ret));
        return ret;
    }

    bool ready = false;
    for (int i = 0; i < SEN5X_READY_POLLS; ++i) {
        ret = sen5x_read_data_ready(&data_ready);
        if (ret != ESP_OK) {
            set_reason(reason, reason_len, "falló data_ready SEN5x: %s", esp_err_to_name(ret));
            return ret;
        }

        if (data_ready == 1) {
            ready = true;
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(SEN5X_READY_POLL_DELAY_MS));
    }

    if (!ready) {
        set_reason(reason, reason_len, "timeout esperando SEN5x data_ready");
        return ESP_ERR_TIMEOUT;
    }

    ret = sen5x_read_measured_values(buf, sizeof(buf));
    if (ret != ESP_OK) {
        set_reason(reason, reason_len, "falló lectura SEN5x: %s", esp_err_to_name(ret));
        return ret;
    }

    float pm1 = 0.0f, pm25 = 0.0f, pm4 = 0.0f, pm10 = 0.0f;
    float rh = 0.0f, temp = 0.0f, voc = 0.0f, nox = 0.0f;

    if (!sen5x_decode_measurement(buf, &pm1, &pm25, &pm4, &pm10, &rh, &temp, &voc, &nox)) {
        set_reason(reason, reason_len, "CRC inválido en SEN5x");
        return ESP_ERR_INVALID_CRC;
    }

    out->co2      = co2;
    out->scd_temp = scd_temp;
    out->scd_hum  = scd_hum;

    out->pm1p0    = pm1;
    out->pm2p5    = pm25;
    out->pm4p0    = pm4;
    out->pm10p0   = pm10;
    out->voc      = voc;
    out->nox      = nox;
    out->sen_temp = temp;
    out->sen_hum  = rh;

    out->avg_temp = (scd_temp + temp) / 2.0f;
    out->avg_hum  = (scd_hum + rh) / 2.0f;

    if (!validate_ranges(out, reason, reason_len)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (s_has_last_valid &&
        !validate_step_change(out, &s_last_valid, reason, reason_len)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

// ---------- API ----------
esp_err_t sensors_init_all(void) {
    ESP_LOGI(TAG_SENS, "Init I2C + sensors...");
    vTaskDelay(pdMS_TO_TICKS(1500));

    if (s_i2c_bus) {
        ESP_LOGD(TAG_SENS, "I2C bus ya inicializado");
        return ESP_OK;
    }

    vTaskDelay(pdMS_TO_TICKS(1500));

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true
        }
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK) return ret;

    i2c_device_config_t scd_cfg = {
        .device_address = SCD4X_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(s_i2c_bus, &scd_cfg, &s_scd4x_dev);
    if (ret != ESP_OK) return ret;

    i2c_device_config_t sen_cfg = {
        .device_address = SEN5X_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(s_i2c_bus, &sen_cfg, &s_sen5x_dev);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(200));
    sen5x_device_reset();
    vTaskDelay(pdMS_TO_TICKS(100));
    sen5x_start_measurement();
    vTaskDelay(pdMS_TO_TICKS(50));
    scd4x_start_measurement();
    vTaskDelay(pdMS_TO_TICKS(5000));

    s_has_last_valid = false;
    memset(&s_last_valid, 0, sizeof(s_last_valid));

    return ESP_OK;
}

esp_err_t sensors_read(SensorData *out) {
    if (!out) return ESP_ERR_INVALID_ARG;

    esp_err_t last_err = ESP_FAIL;
    char reason[96] = {0};

    for (int attempt = 1; attempt <= SENSOR_READ_ATTEMPTS; ++attempt) {
        SensorData candidate = {0};
        reason[0] = '\0';

        last_err = sensors_read_raw(&candidate, reason, sizeof(reason));
        if (last_err == ESP_OK) {
            *out = candidate;
            s_last_valid = candidate;
            s_has_last_valid = true;
            return ESP_OK;
        }

        ESP_LOGW(TAG_SENS,
                 "Lectura rechazada intento %d/%d: %s (%s)",
                 attempt,
                 SENSOR_READ_ATTEMPTS,
                 reason[0] ? reason : "sin detalle",
                 esp_err_to_name(last_err));

        if (attempt < SENSOR_READ_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(SENSOR_CONFIRM_DELAY_MS));
        }
    }

    return last_err;
}

void sensors_format_json(const SensorData *d,
                         const char *time_str,
                         const char *fecha_str,
                         const char *inicio_str,
                         char *buf,
                         size_t buf_size) {
    if (!buf || buf_size == 0 || !d) return;

    int written = snprintf(
        buf, buf_size,
        "{\"pm1p0\":%.2f,\"pm2p5\":%.2f,\"pm4p0\":%.2f,\"pm10p0\":%.2f,"
        "\"voc\":%.1f,\"nox\":%.1f,\"cTe\":%.2f,\"cHu\":%.2f,\"co2\":%u,"
        "\"fecha\":\"%s\",\"inicio\":\"%s\",\"ciudad\":\"%s\","
        "\"hora\":\"%s\",\"device_id\":\"%s\"}",
        d->pm1p0, d->pm2p5, d->pm4p0, d->pm10p0,
        d->voc, d->nox, d->avg_temp, d->avg_hum, d->co2,
        fecha_str, inicio_str, g_city_state, time_str, DEVICE_ID
    );

    if (written < 0 || (size_t)written >= buf_size) {
        buf[buf_size - 1] = '\0';
    }
}

void sensors_set_city_state(const char *city_state) {
    if (!city_state) return;

    size_t len = strlen(city_state);
    if (len >= sizeof(g_city_state)) len = sizeof(g_city_state) - 1;

    memcpy(g_city_state, city_state, len);
    g_city_state[len] = '\0';
}