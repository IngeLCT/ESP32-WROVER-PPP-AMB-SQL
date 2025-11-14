#include "app.h"
#include "rtdb.h"
#include <string>

// Acceso a claves privadas centralizadas
#include "Privado.h"

using namespace ESPFirebase;

static FirebaseApp* g_app = nullptr;
static RTDB* g_rtdb = nullptr;

extern "C" {

int firebase_init(void) {
	if (g_app) return 0;
	// Create Firebase app with API key
	g_app = new FirebaseApp(API_KEY);

	// Login using email/password
	g_app->user_account = { USER_EMAIL, USER_PASSWORD };
	esp_err_t err = g_app->loginUserAccount(g_app->user_account);
	if (err != ESP_OK) {
		// Try register then login as fallback
		if (g_app->registerUserAccount(g_app->user_account) == ESP_OK) {
			err = g_app->loginUserAccount(g_app->user_account);
		}
	}
	if (err != ESP_OK) return -2;

	// Create RTDB client
	g_rtdb = new RTDB(g_app, DATABASE_URL);
	return 0;
}

int firebase_refresh_token(void) {
	if (!g_app) return -1;
	// Forzamos refresh usando el refresh_token almacenado;
	// si falla, intenta login completo.
	if (g_app->forceRefreshAuth() == ESP_OK) return 0;
	return -2;
}

int firebase_push(const char* path, const char* json) {
	if (!g_rtdb) return -1;
	// RTDB::postData corresponds to push semantics
	esp_err_t err = g_rtdb->postData(path, json);
	return err == ESP_OK ? 0 : (int)err;
}

int firebase_putData(const char* path, const char* json) {
	if (!g_rtdb) return -1;
	esp_err_t err = g_rtdb->putData(path, json);
	return err == ESP_OK ? 0 : (int)err;
}

int firebase_delete(const char* path) {
	if (!g_rtdb) return -1;
	esp_err_t err = g_rtdb->deleteData(path);
	return err == ESP_OK ? 0 : (int)err;
}

int firebase_trim_days(const char* root_path, int max_days) {
    if (!g_rtdb) return -1;
    esp_err_t err = g_rtdb->trimDays(root_path, max_days);
    return err == ESP_OK ? 0 : (int)err;
}

int firebase_trim_oldest_batch(const char* root_path, int batch_size) {
    if (!g_rtdb) return -1;
    return g_rtdb->trimOldestBatch(root_path, batch_size);
}

}

