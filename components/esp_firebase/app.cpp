
#include <iostream>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"

#include "app.h"



#include "value.h"
#include "json.h"

// #include "nvs.h"
// #include "nvs_flash.h"
#define NVS_TAG "NVS"


#define HTTP_TAG "HTTP_CLIENT"
#define FIREBASE_APP_TAG "FirebaseApp"

// Prefer ESP-IDF certificate bundle over embedded certs



static int output_len = 0; 
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{

    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_CONNECTED");
            memset(evt->user_data, 0, HTTP_RECV_BUFFER_SIZE);
            output_len = 0;
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(HTTP_TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_REDIRECT:
            ESP_LOGD(HTTP_TAG, "HTTP_EVENT_REDIRECT");
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_FINISH");
            output_len = 0;
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(HTTP_TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (evt->user_data && evt->data && evt->data_len > 0) {
                int capacity = HTTP_RECV_BUFFER_SIZE - 1; // deja 1 para terminador
                int space = capacity - output_len;
                if (space > 0) {
                    int to_copy = evt->data_len < space ? evt->data_len : space;
                    memcpy((char*)evt->user_data + output_len, evt->data, to_copy);
                    output_len += to_copy;
                    ((char*)evt->user_data)[output_len] = '\0';
                }
            }
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(HTTP_TAG, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}

namespace ESPFirebase {

// TODO: protect this function from breaking 
void FirebaseApp::firebaseClientInit(void)
{   
    esp_http_client_config_t config = {};
    config.url = "https://google.com";    // you have to set this as https link of some sort so that it can init properly, you cant leave it empty
    config.event_handler = http_event_handler;
    // Use global certificate bundle (requires CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.user_data = FirebaseApp::local_response_buffer;
    config.buffer_size_tx = 4096;
    config.buffer_size = HTTP_RECV_BUFFER_SIZE;
    // Timeout razonable (ms)
    config.timeout_ms = 20000; // 20s
    this->default_timeout_ms = config.timeout_ms;  // 20 s por defecto
    // Deshabilitamos keep-alive: el server parece cerrar tras inactividad (~10 min) provocando RST en primer write
    config.keep_alive_enable = false;
    FirebaseApp::client = esp_http_client_init(&config);
    ESP_LOGD(FIREBASE_APP_TAG, "HTTP Client Initialized");

}


esp_err_t FirebaseApp::setHeader(const char* header, const char* value)
{
    return esp_http_client_set_header(FirebaseApp::client, header, value);
}

http_ret_t FirebaseApp::performRequest(const char* url,
                                       esp_http_client_method_t method,
                                       std::string post_field)
{
    const int MAX_ATTEMPTS = 5; // 1 intento + 1 reintento
    esp_err_t err = ESP_FAIL;
    int status_code = -1;

    for (int attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt) {
        // Inicializa o reusa el cliente
        if (FirebaseApp::client == nullptr) {
            esp_http_client_config_t cfg = {};
            cfg.url = url;
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
            FirebaseApp::client = esp_http_client_init(&cfg);
            if (!FirebaseApp::client) {
                ESP_LOGE(FIREBASE_APP_TAG, "http_client_init fallo");
                break;
            }
        } else {
            esp_http_client_set_url(FirebaseApp::client, url);
        }

        if (esp_http_client_set_method(FirebaseApp::client, method) != ESP_OK) {
            ESP_LOGE(FIREBASE_APP_TAG, "set_method fallo");
        }

        // Métodos con body
        if (method == HTTP_METHOD_POST || method == HTTP_METHOD_PUT || method == HTTP_METHOD_PATCH) {
            if (esp_http_client_set_post_field(FirebaseApp::client,
                                               post_field.c_str(),
                                               post_field.length()) != ESP_OK) {
                ESP_LOGE(FIREBASE_APP_TAG, "set_post_field fallo");
            }
            setHeader("content-type", "application/json");
        } else {
            // Métodos SIN body (DELETE/GET): limpiar payload y forzar Content-Length: 0
            esp_http_client_set_post_field(FirebaseApp::client, "", 0);
            esp_http_client_set_header(FirebaseApp::client, "Content-Length", "0");
        }

        err = esp_http_client_perform(FirebaseApp::client);
        status_code = esp_http_client_get_status_code(FirebaseApp::client);

        // Aceptar cualquier 2xx como éxito (DELETE puede devolver 204)
        if (err == ESP_OK && status_code >= 200 && status_code < 300) {
            esp_http_client_close(FirebaseApp::client);
            return {err, status_code};
        }

        ESP_LOGE(FIREBASE_APP_TAG,
                "request: url=%s\nmethod=%d\npost_field=%s",
                url, method, post_field.c_str());
        ESP_LOGE(FIREBASE_APP_TAG, "response=\n%s", local_response_buffer);

        // Reintento: asegurar headers/estado del body correctos
        if (method == HTTP_METHOD_POST || method == HTTP_METHOD_PUT || method == HTTP_METHOD_PATCH) {
            setHeader("content-type", "application/json");
        } else {
            esp_http_client_set_post_field(FirebaseApp::client, "", 0);
            esp_http_client_set_header(FirebaseApp::client, "Content-Length", "0");
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return {err, status_code};
}


void FirebaseApp::clearHTTPBuffer(void)
{   
    memset(FirebaseApp::local_response_buffer, 0, HTTP_RECV_BUFFER_SIZE);
    output_len = 0;
}

void FirebaseApp::setHttpTimeoutMs(int ms) {
    if (this->client) esp_http_client_set_timeout_ms(this->client, ms);
}

void FirebaseApp::restoreDefaultHttpTimeout() {
    if (this->client) esp_http_client_set_timeout_ms(this->client, this->default_timeout_ms);
}


esp_err_t FirebaseApp::getRefreshToken(bool register_account)
{


    http_ret_t http_ret;
    
    std::string account_json = R"({"email":")";
    account_json += FirebaseApp::user_account.user_email; 
    account_json += + R"(", "password":")"; 
    account_json += FirebaseApp::user_account.user_password;
    account_json += R"(", "returnSecureToken": true})"; 

    FirebaseApp::setHeader("content-type", "application/json");
    if (register_account)
    {
        http_ret = FirebaseApp::performRequest(FirebaseApp::register_url.c_str(), HTTP_METHOD_POST, account_json);
    }
    else
    {
        http_ret = FirebaseApp::performRequest(FirebaseApp::login_url.c_str(), HTTP_METHOD_POST, account_json);
    }

    if (http_ret.err == ESP_OK && http_ret.status_code == 200)
    {
        // std::cout << FirebaseApp::local_response_buffer << '\n';
        const char* begin = FirebaseApp::local_response_buffer;
        const char* end = begin + strlen(FirebaseApp::local_response_buffer);
        Json::Reader reader;
        Json::Value data;
        reader.parse(begin, end, data, false);
        FirebaseApp::refresh_token = data["refreshToken"].asString();

        ESP_LOGD(FIREBASE_APP_TAG, "Refresh Token=%s", FirebaseApp::refresh_token.c_str());
        return ESP_OK;
    }
    else 
    {
        return ESP_FAIL;
    }
}
esp_err_t FirebaseApp::getAuthToken()
{
    http_ret_t http_ret;

    std::string token_post_data = R"({"grant_type": "refresh_token", "refresh_token":")";
    token_post_data+= FirebaseApp::refresh_token + "\"}";


    FirebaseApp::setHeader("content-type", "application/json");
    http_ret = FirebaseApp::performRequest(FirebaseApp::auth_url.c_str(), HTTP_METHOD_POST, token_post_data);
    if (http_ret.err == ESP_OK && http_ret.status_code == 200)
    {
        const char* begin = FirebaseApp::local_response_buffer;
        const char* end = begin + strlen(FirebaseApp::local_response_buffer);
        Json::Reader reader;
        Json::Value data;
        reader.parse(begin, end, data, false);
        FirebaseApp::auth_token = data["access_token"].asString();
        // expires_in llega como string en segundos
        if (data.isMember("expires_in")) {
            FirebaseApp::auth_expires_in = atoi(data["expires_in"].asCString());
        } else if (data.isMember("expiresIn")) { // por si cambia el campo
            FirebaseApp::auth_expires_in = atoi(data["expiresIn"].asCString());
        } else {
            FirebaseApp::auth_expires_in = 3600; // fallback 1h
        }
        FirebaseApp::auth_obtained_time = time(NULL);

        ESP_LOGI(FIREBASE_APP_TAG, "Auth Token acquired (expira en %d s)", FirebaseApp::auth_expires_in);
        return ESP_OK;
    }
    else {
        return ESP_FAIL;
    }

    
}

FirebaseApp::FirebaseApp(const char* api_key)
    : api_key(api_key)
{
    
    FirebaseApp::local_response_buffer = new char[HTTP_RECV_BUFFER_SIZE];
    FirebaseApp::register_url += FirebaseApp::api_key; 
    FirebaseApp::login_url += FirebaseApp::api_key;
    FirebaseApp::auth_url += FirebaseApp::api_key;
    firebaseClientInit();
}

FirebaseApp::~FirebaseApp()
{
    delete[] FirebaseApp::local_response_buffer;
    esp_http_client_cleanup(FirebaseApp::client);
}

esp_err_t FirebaseApp::registerUserAccount(const user_account_t& account)
{
    if (FirebaseApp::user_account.user_email != account.user_email || FirebaseApp::user_account.user_password != account.user_password)
    {
        FirebaseApp::user_account.user_email = account.user_email;
        FirebaseApp::user_account.user_password = account.user_password;
    }
    esp_err_t err = FirebaseApp::getRefreshToken(true);
    if (err != ESP_OK)
    {
        ESP_LOGE(FIREBASE_APP_TAG, "Failed to get refresh token");
        return ESP_FAIL;
    }
    FirebaseApp::clearHTTPBuffer();
    err = FirebaseApp::getAuthToken();
    if (err != ESP_OK)
    {
        ESP_LOGE(FIREBASE_APP_TAG, "Failed to get auth token");
        return ESP_FAIL;
    }
    FirebaseApp::clearHTTPBuffer();
    ESP_LOGI(FIREBASE_APP_TAG, "Created user successfully");

    return ESP_OK;
}

esp_err_t FirebaseApp::loginUserAccount(const user_account_t& account)
{
    if (FirebaseApp::user_account.user_email != account.user_email || FirebaseApp::user_account.user_password != account.user_password)
    {
        FirebaseApp::user_account.user_email = account.user_email;
        FirebaseApp::user_account.user_password = account.user_password;
    }
    esp_err_t err = FirebaseApp::getRefreshToken(false);
    if (err != ESP_OK)
    {
        ESP_LOGE(FIREBASE_APP_TAG, "Failed to get refresh token");
        return ESP_FAIL;
    }
    FirebaseApp::clearHTTPBuffer();

    err = FirebaseApp::getAuthToken();
    if (err != ESP_OK)
    {
        ESP_LOGE(FIREBASE_APP_TAG, "Failed to get auth token");
        return ESP_FAIL;
    }
    FirebaseApp::clearHTTPBuffer();
    ESP_LOGI(FIREBASE_APP_TAG, "Login to user successful");
    return ESP_OK;
}

esp_err_t FirebaseApp::refreshAuthIfNeeded()
{
    if (FirebaseApp::auth_token.empty()) {
        ESP_LOGW(FIREBASE_APP_TAG, "No auth token yet, logging in again");
        return FirebaseApp::loginUserAccount(FirebaseApp::user_account);
    }
    time_t now = time(NULL);
    if (FirebaseApp::auth_expires_in <= 0) return ESP_OK; // sin info, confiar
    int elapsed = (int)(now - FirebaseApp::auth_obtained_time);
    int remaining = FirebaseApp::auth_expires_in - elapsed;
    if (remaining < 30) { // menos de 30s, renovar
        ESP_LOGI(FIREBASE_APP_TAG, "Auth token a punto de expirar (%d s). Renovando...", remaining);
        if (FirebaseApp::getAuthToken() == ESP_OK) {
            FirebaseApp::clearHTTPBuffer();
            return ESP_OK;
        } else {
            ESP_LOGW(FIREBASE_APP_TAG, "Fallo refresh directo, intentando login completo");
            esp_err_t err = FirebaseApp::loginUserAccount(FirebaseApp::user_account);
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t FirebaseApp::forceRefreshAuth()
{
    ESP_LOGI(FIREBASE_APP_TAG, "Forzando refresh de auth token...");
    if (FirebaseApp::getAuthToken() == ESP_OK) {
        FirebaseApp::clearHTTPBuffer();
        return ESP_OK;
    }
    ESP_LOGW(FIREBASE_APP_TAG, "Fallo refresh directo, intentando login completo");
    return FirebaseApp::loginUserAccount(FirebaseApp::user_account);
}


}
