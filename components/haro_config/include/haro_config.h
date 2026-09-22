#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

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

// Speaker volume level, 1-10 (main.c maps this to audio_pipeline_set_
// volume_percent()'s 0-100 scale as level*10) -- set via the KEY1/KEY3
// buttons. *out is left unset on failure. Same "not written yet" fallback
// pattern as haro_config_get_server_url() above: factory-fresh NVS falls
// back to 8 (== 80%, the volume this board shipped with before per-user
// adjustment existed) rather than returning ESP_ERR_NVS_NOT_FOUND.
esp_err_t haro_config_get_volume_level(uint8_t *out);
esp_err_t haro_config_set_volume_level(uint8_t level);

#ifdef __cplusplus
}
#endif
