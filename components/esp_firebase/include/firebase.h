#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int firebase_init(void);
int firebase_auth(void);
int firebase_refresh_token(void);
int firebase_push(const char* path, const char* json);
int firebase_putData(const char* path, const char* json);
int firebase_delete(const char* path);
int firebase_trim_days(const char* root_path, int max_days);
int firebase_trim_oldest_batch(const char* root_path, int batch_size);

#ifdef __cplusplus
}
#endif
