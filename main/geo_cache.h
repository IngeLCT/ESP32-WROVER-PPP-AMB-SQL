#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GEO_CACHE_CITY_MAX_LEN 64

typedef struct {
    char last_city[GEO_CACHE_CITY_MAX_LEN];
    bool geo_attempt_done_today;
    bool geo_rate_limited_today;
} geo_cache_state_t;

esp_err_t geo_cache_load(geo_cache_state_t *state);
esp_err_t geo_cache_store_last_city(const char *city);
esp_err_t geo_cache_set_daily_flags(bool geo_attempt_done_today,
                                    bool geo_rate_limited_today);
esp_err_t geo_cache_reset_daily_flags(void);

#ifdef __cplusplus
}
#endif
