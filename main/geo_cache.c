#include "geo_cache.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "geo_cache";

#define GEO_CACHE_NAMESPACE      "geo_cache"
#define GEO_CACHE_KEY_LAST_CITY  "last_city"
#define GEO_CACHE_KEY_ATTEMPT    "attempt_count"
#define GEO_CACHE_KEY_LEGACY_ATTEMPT "attempt_done"
#define GEO_CACHE_KEY_SUCCESS    "success_today"
#define GEO_CACHE_KEY_RATE_LIMIT "rate_limited"

static esp_err_t geo_cache_open(nvs_handle_t *handle, nvs_open_mode_t mode)
{
    return nvs_open(GEO_CACHE_NAMESPACE, mode, handle);
}

esp_err_t geo_cache_load(geo_cache_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(*state));

    nvs_handle_t handle;
    esp_err_t err = geo_cache_open(&handle, NVS_READONLY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo abrir NVS para leer geo cache: %s",
                 esp_err_to_name(err));
        return err;
    }

    size_t city_len = sizeof(state->last_city);
    err = nvs_get_str(handle, GEO_CACHE_KEY_LAST_CITY, state->last_city, &city_len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No se pudo leer last_city: %s", esp_err_to_name(err));
    }

    uint8_t success_today = 0;
    err = nvs_get_u8(handle, GEO_CACHE_KEY_SUCCESS, &success_today);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No se pudo leer success_today: %s", esp_err_to_name(err));
    }

    uint8_t rate_limited = 0;
    err = nvs_get_u8(handle, GEO_CACHE_KEY_RATE_LIMIT, &rate_limited);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No se pudo leer rate_limited: %s", esp_err_to_name(err));
    }

    uint8_t attempt_count = 0;
    err = nvs_get_u8(handle, GEO_CACHE_KEY_ATTEMPT, &attempt_count);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t legacy_attempt_done = 0;
        err = nvs_get_u8(handle, GEO_CACHE_KEY_LEGACY_ATTEMPT, &legacy_attempt_done);
        if (err == ESP_OK) {
            attempt_count = legacy_attempt_done ? 1 : 0;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "No se pudo leer attempt_done legacy: %s", esp_err_to_name(err));
        }
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo leer attempt_count: %s", esp_err_to_name(err));
    }

    state->geo_success_today = (success_today != 0);
    state->geo_rate_limited_today = (rate_limited != 0);
    state->geo_attempt_count_today = attempt_count;

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t geo_cache_store_last_city(const char *city)
{
    if (!city) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = geo_cache_open(&handle, NVS_READWRITE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo abrir NVS para guardar ciudad: %s",
                 esp_err_to_name(err));
        return err;
    }

    char current_city[GEO_CACHE_CITY_MAX_LEN] = "";
    size_t current_len = sizeof(current_city);
    err = nvs_get_str(handle, GEO_CACHE_KEY_LAST_CITY, current_city, &current_len);
    if (err == ESP_OK && strcmp(current_city, city) == 0) {
        nvs_close(handle);
        return ESP_OK;
    }

    err = nvs_set_str(handle, GEO_CACHE_KEY_LAST_CITY, city);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo guardar last_city: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t geo_cache_set_daily_state(bool geo_success_today,
                                    bool geo_rate_limited_today,
                                    uint8_t geo_attempt_count_today)
{
    nvs_handle_t handle;
    esp_err_t err = geo_cache_open(&handle, NVS_READWRITE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo abrir NVS para guardar flags diarios: %s",
                 esp_err_to_name(err));
        return err;
    }

    uint8_t current_success = 0;
    uint8_t current_rate_limit = 0;
    uint8_t current_attempt = 0;
    (void)nvs_get_u8(handle, GEO_CACHE_KEY_SUCCESS, &current_success);
    (void)nvs_get_u8(handle, GEO_CACHE_KEY_RATE_LIMIT, &current_rate_limit);
    (void)nvs_get_u8(handle, GEO_CACHE_KEY_ATTEMPT, &current_attempt);

    uint8_t new_success = geo_success_today ? 1 : 0;
    uint8_t new_rate_limit = geo_rate_limited_today ? 1 : 0;
    uint8_t new_attempt = geo_attempt_count_today;

    if (current_success == new_success &&
        current_rate_limit == new_rate_limit &&
        current_attempt == new_attempt) {
        nvs_close(handle);
        return ESP_OK;
    }

    err = nvs_set_u8(handle, GEO_CACHE_KEY_SUCCESS, new_success);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, GEO_CACHE_KEY_RATE_LIMIT, new_rate_limit);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, GEO_CACHE_KEY_ATTEMPT, new_attempt);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudieron guardar flags diarios: %s",
                 esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t geo_cache_reset_daily_state(void)
{
    return geo_cache_set_daily_state(false, false, 0);
}
