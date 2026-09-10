#pragma once
#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t haro_config_init(void);
esp_err_t haro_config_get_server_url(char *out, size_t out_len);
esp_err_t haro_config_set_server_url(const char *url);

#ifdef __cplusplus
}
#endif
