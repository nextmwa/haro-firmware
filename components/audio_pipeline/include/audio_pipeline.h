#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_pipeline_init(void);
esp_err_t audio_pipeline_read(void *buf, size_t len, size_t *bytes_read);
esp_err_t audio_pipeline_write(const void *buf, size_t len);

// Returns the I2C bus (I2C_NUM_0, GPIO10=SCL/GPIO11=SDA) audio_pipeline_init()
// created for the onboard ES8311/ES7210/TCA9555 codecs. Found on real
// hardware: this is the SAME bus this board's 18-pin external header exposes
// as its "SDA"/"SCL" pins (silkscreen-labeled, physically GPIO11/GPIO10) --
// not a second, independent bus. Any external I2C peripheral wired to that
// header (e.g. face_display's SSD1306) must attach to THIS bus handle via
// esp_lcd_new_panel_io_i2c() et al, rather than creating its own with
// i2c_new_master_bus() on the same pins, which would fail (a pin can only be
// claimed by one bus). Valid only after audio_pipeline_init() has returned
// ESP_OK; call this after, never before.
esp_err_t audio_pipeline_get_i2c_bus(i2c_master_bus_handle_t *out_bus);

#ifdef __cplusplus
}
#endif
