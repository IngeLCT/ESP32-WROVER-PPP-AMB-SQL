#ifndef _ESP_FIREBASE_RTDB_H_
#define  _ESP_FIREBASE_RTDB_H_
#include "app.h"


#include "value.h"
#include "json.h"

namespace ESPFirebase 
{

    
    
    class RTDB
    {
    private:
        FirebaseApp* app;
        std::string base_database_url;


    public:
                
        Json::Value getData(const char* path);

        esp_err_t putData(const char* path, const char* json_str);
        esp_err_t putData(const char* path, const Json::Value& data);

        esp_err_t postData(const char* path, const char* json_str);
        esp_err_t postData(const char* path, const Json::Value& data);

        esp_err_t patchData(const char* path, const char* json_str);
        esp_err_t patchData(const char* path, const Json::Value& data);
        
        esp_err_t deleteData(const char* path);
        // Opcionales de mantenimiento
        esp_err_t trimDays(const char* root_path, int max_days);
        int trimOldestBatch(const char* root_path, int batch_size);
        RTDB(FirebaseApp* app, const char* database_url);
    };




}


#endif
