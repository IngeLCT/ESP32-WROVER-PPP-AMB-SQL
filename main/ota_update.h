#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void ota_check_and_update_if_needed(void);

const char *ota_update_get_manifest_url(void);

#ifdef __cplusplus
}
#endif
