#include "ota_update.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "OTA_UPDATE";

#define OTA_MANIFEST_URL "https://ambiental-lct.ecosensor.com.mx/firmware/manifest.json"
#define OTA_HTTP_TIMEOUT_MS 15000
#define OTA_MAX_ATTEMPTS 5
#define OTA_RETRY_DELAY_MS 2000

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} http_buffer_t;

typedef struct {
    char version[32];
    char firmware_url[256];
    char release_date[32];
    bool enabled;
    bool has_version;
    bool has_firmware_url;
    bool has_enabled;
} ota_manifest_t;

typedef enum {
    OTA_CHECK_RESULT_DONE = 0,
    OTA_CHECK_RESULT_RETRY,
} ota_check_result_t;

static esp_err_t http_buffer_append(http_buffer_t *buffer, const char *data, int data_len) {
    if (!buffer || !data || data_len <= 0) {
        return ESP_OK;
    }

    size_t required = buffer->len + (size_t)data_len + 1;
    if (required > buffer->cap) {
        size_t new_cap = (buffer->cap == 0) ? 256 : buffer->cap;
        while (new_cap < required) {
            new_cap *= 2;
        }

        char *new_data = realloc(buffer->data, new_cap);
        if (!new_data) {
            ESP_LOGE(TAG, "Sin memoria para buffer HTTP (%u bytes)", (unsigned)new_cap);
            return ESP_ERR_NO_MEM;
        }

        buffer->data = new_data;
        buffer->cap = new_cap;
    }

    memcpy(buffer->data + buffer->len, data, (size_t)data_len);
    buffer->len += (size_t)data_len;
    buffer->data[buffer->len] = '\0';
    return ESP_OK;
}

static esp_err_t manifest_http_event_handler(esp_http_client_event_t *evt) {
    http_buffer_t *buffer = (http_buffer_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        return http_buffer_append(buffer, (const char *)evt->data, evt->data_len);
    }

    return ESP_OK;
}

static esp_err_t http_get_manifest(char **out_body) {
    if (!out_body) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_body = NULL;

    http_buffer_t buffer = {0};
    esp_http_client_config_t config = {
        .url = OTA_MANIFEST_URL,
        .event_handler = manifest_http_event_handler,
        .user_data = &buffer,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fallo consultando manifest: %s", esp_err_to_name(err));
        free(buffer.data);
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Manifest respondio HTTP %d", status);
        free(buffer.data);
        return ESP_FAIL;
    }

    if (!buffer.data || buffer.len == 0) {
        ESP_LOGE(TAG, "Manifest vacio");
        free(buffer.data);
        return ESP_FAIL;
    }

    *out_body = buffer.data;
    return ESP_OK;
}

static bool copy_json_string(cJSON *object, const char *key, char *dst, size_t dstlen) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || !item->valuestring || dstlen == 0) {
        return false;
    }

    snprintf(dst, dstlen, "%s", item->valuestring);
    return true;
}

static esp_err_t parse_manifest(const char *json_body, ota_manifest_t *manifest) {
    if (!json_body || !manifest) {
        return ESP_ERR_INVALID_ARG;
    }

    *manifest = (ota_manifest_t){0};

    cJSON *root = cJSON_Parse(json_body);
    if (!root) {
        ESP_LOGE(TAG, "Manifest JSON invalido");
        return ESP_FAIL;
    }

    manifest->has_version = copy_json_string(root,
                                             "version",
                                             manifest->version,
                                             sizeof(manifest->version));
    manifest->has_firmware_url = copy_json_string(root,
                                                  "firmware_url",
                                                  manifest->firmware_url,
                                                  sizeof(manifest->firmware_url));
    copy_json_string(root,
                     "release_date",
                     manifest->release_date,
                     sizeof(manifest->release_date));

    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (cJSON_IsBool(enabled)) {
        manifest->enabled = cJSON_IsTrue(enabled);
        manifest->has_enabled = true;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static bool parse_semver_triplet(const char *version, int *major, int *minor, int *patch) {
    if (!version || !major || !minor || !patch) {
        return false;
    }

    int parsed = sscanf(version, "%d.%d.%d", major, minor, patch);
    return parsed == 3;
}

static bool compare_semver(const char *remote_version, const char *local_version, int *cmp_out) {
    int remote_major = 0;
    int remote_minor = 0;
    int remote_patch = 0;
    int local_major = 0;
    int local_minor = 0;
    int local_patch = 0;

    if (!cmp_out) {
        return false;
    }

    if (!parse_semver_triplet(remote_version, &remote_major, &remote_minor, &remote_patch) ||
        !parse_semver_triplet(local_version, &local_major, &local_minor, &local_patch)) {
        ESP_LOGW(TAG,
                 "Formato de version no valido. remota=%s local=%s",
                 remote_version ? remote_version : "(null)",
                 local_version ? local_version : "(null)");
        return false;
    }

    if (remote_major != local_major) {
        *cmp_out = (remote_major > local_major) ? 1 : -1;
        return true;
    }
    if (remote_minor != local_minor) {
        *cmp_out = (remote_minor > local_minor) ? 1 : -1;
        return true;
    }
    if (remote_patch != local_patch) {
        *cmp_out = (remote_patch > local_patch) ? 1 : -1;
        return true;
    }

    *cmp_out = 0;
    return true;
}

static void log_time_warning_if_needed(void) {
    time_t now = 0;
    time(&now);
    if (now < 1609459200) {
        ESP_LOGW(TAG, "Hora del sistema no validada aun; HTTPS puede fallar si el certificado requiere fecha valida");
    }
}

static esp_err_t perform_https_ota(const char *firmware_url) {
    esp_http_client_config_t http_config = {
        .url = firmware_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    ESP_LOGI(TAG, "Iniciando OTA desde %s", firmware_url);
    return esp_https_ota(&ota_config);
}

const char *ota_update_get_manifest_url(void) {
    return OTA_MANIFEST_URL;
}

static ota_check_result_t ota_check_and_update_once(void) {
    char *manifest_body = NULL;
    ota_manifest_t manifest = {0};

    log_time_warning_if_needed();
    ESP_LOGI(TAG, "Consultando manifest OTA: %s", OTA_MANIFEST_URL);

    esp_err_t err = http_get_manifest(&manifest_body);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo obtener el manifest");
        return OTA_CHECK_RESULT_RETRY;
    }

    err = parse_manifest(manifest_body, &manifest);
    free(manifest_body);
    manifest_body = NULL;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Manifest invalido");
        return OTA_CHECK_RESULT_RETRY;
    }

    ESP_LOGI(TAG,
             "Manifest recibido: version=%s enabled=%s release_date=%s",
             manifest.has_version ? manifest.version : "(missing)",
             (manifest.has_enabled && manifest.enabled) ? "true" : "false",
             manifest.release_date[0] ? manifest.release_date : "(missing)");

    if (!manifest.has_enabled || !manifest.has_version || !manifest.has_firmware_url) {
        ESP_LOGW(TAG, "Manifest incompleto");
        return OTA_CHECK_RESULT_RETRY;
    }

    if (!manifest.enabled) {
        ESP_LOGI(TAG, "OTA deshabilitado en manifest. Fin de revision OTA");
        return OTA_CHECK_RESULT_DONE;
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *local_version = (app_desc && app_desc->version[0]) ? app_desc->version : "0.0.0";

    ESP_LOGI(TAG, "Version local: %s", local_version);
    ESP_LOGI(TAG, "Version remota: %s", manifest.version);

    int cmp = 0;
    if (!compare_semver(manifest.version, local_version, &cmp)) {
        return OTA_CHECK_RESULT_RETRY;
    }

    if (cmp <= 0) {
        ESP_LOGI(TAG, "No hay actualizacion disponible. Fin de revision OTA");
        return OTA_CHECK_RESULT_DONE;
    }

    ESP_LOGI(TAG, "Actualizacion disponible. OTA habilitado=true");
    err = perform_https_ota(manifest.firmware_url);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Fallo OTA: %s", esp_err_to_name(err));
        return OTA_CHECK_RESULT_RETRY;
    }

    ESP_LOGI(TAG, "OTA exitosa. Reiniciando ESP32");
    esp_restart();
    return OTA_CHECK_RESULT_DONE;
}

void ota_check_and_update_if_needed(void) {
    for (int attempt = 1; attempt <= OTA_MAX_ATTEMPTS; ++attempt) {
        ESP_LOGI(TAG, "OTA intento %d/%d", attempt, OTA_MAX_ATTEMPTS);

        ota_check_result_t result = ota_check_and_update_once();
        if (result == OTA_CHECK_RESULT_DONE) {
            return;
        }

        if (attempt < OTA_MAX_ATTEMPTS) {
            ESP_LOGW(TAG,
                     "Revision OTA fallida. Reintentando en %d ms",
                     OTA_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(OTA_RETRY_DELAY_MS));
        }
    }

    ESP_LOGW(TAG,
             "No se pudo revisar/aplicar OTA tras %d intentos. Continuo operacion normal",
             OTA_MAX_ATTEMPTS);
}
