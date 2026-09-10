#pragma once
#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t haro_config_init(void);

// Reads the server URL from NVS into out (out_len bytes, NUL-terminated).
// If no URL has been written yet (factory-fresh NVS, i.e. before anything
// has called haro_config_set_server_url()), falls back to the compile-time
// CONFIG_HARO_DEFAULT_SERVER_URL Kconfig default instead of returning
// ESP_ERR_NVS_NOT_FOUND -- see components/haro_config/Kconfig. Still
// returns other NVS errors (e.g. ESP_ERR_NVS_INVALID_HANDLE) or
// ESP_ERR_INVALID_SIZE (out_len too small for the stored/default value) as
// real failures.
esp_err_t haro_config_get_server_url(char *out, size_t out_len);
esp_err_t haro_config_set_server_url(const char *url);

#ifdef __cplusplus
}
#endif
