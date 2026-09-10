#pragma once
#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_pipeline_init(void);
esp_err_t audio_pipeline_read(void *buf, size_t len, size_t *bytes_read);
esp_err_t audio_pipeline_write(const void *buf, size_t len);
esp_err_t audio_pipeline_set_out_volume(int volume);

#ifdef __cplusplus
}
#endif
