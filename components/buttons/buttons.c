// buttons: reads KEY1/KEY2/KEY3 from the onboard TCA9555 I2C GPIO expander
// (same chip already used in audio_pipeline.c for the speaker amp enable
// and camera_face_track.c for camera PWDN/RESET -- see those files for the
// register-map background, repeated here only where it differs).
#include "buttons.h"
#include "esp_log.h"

static const char *TAG = "buttons";

// Same address as every other TCA9555 use in this project (0x20, inferred
// from the schematic's address-pin strapping, not board-silkscreen text --
// see audio_pipeline.c's comment on this).
#define TCA9555_I2C_ADDR 0x20
// Input Port 1 register (standard TCA9555 register map: 0x00/0x01 = Input
// Port 0/1). KEY1/KEY2/KEY3 = EXIO9/EXIO10/EXIO11 per the board's pinout
// diagram, i.e. port 1 bits 1/2/3 (EXIO8-15 = port 1 bits 0-7, matching
// audio_pipeline.c's own EXIO8=port1 bit0 comment for PA_EN).
#define TCA9555_REG_INPUT_PORT1 0x01
#define KEY1_BIT (1 << 1)
#define KEY2_BIT (1 << 2)
#define KEY3_BIT (1 << 3)

static i2c_master_dev_handle_t s_dev;
// Bit pattern from the PREVIOUS poll, for edge detection -- all-1s
// (unpressed, active-low) until the first real read succeeds, so a button
// already held down at boot is not misreported as a fresh press on the
// very first poll.
static uint8_t s_prev_state = 0xFF;

esp_err_t buttons_init(i2c_master_bus_handle_t i2c_bus)
{
    esp_err_t err = i2c_master_probe(i2c_bus, TCA9555_I2C_ADDR, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TCA9555 not found at 0x%02X (%s) -- buttons unavailable",
                 TCA9555_I2C_ADDR, esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9555_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    // Kept open for the life of the program (unlike audio_pipeline.c's/
    // camera_face_track.c's TCA9555 helpers, which add-then-remove their
    // device handle since they each run once at boot) -- buttons_poll()
    // is called repeatedly, so re-adding/removing the handle on every
    // poll would be wasted I2C-bus bookkeeping for no benefit.
    err = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to add TCA9555 device: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "buttons initialized (KEY1-3 on TCA9555 EXIO9-11)");
    return ESP_OK;
}

bool buttons_poll(button_id_t *out_button)
{
    if (s_dev == NULL) {
        return false;
    }

    uint8_t reg = TCA9555_REG_INPUT_PORT1;
    uint8_t state;
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, &state, 1, 100);
    if (err != ESP_OK) {
        // Transient I2C glitch -- log and treat as "no press" rather than
        // propagate; a dropped button poll every so often is a non-issue,
        // and leaving s_prev_state unchanged means the next successful
        // poll still compares against the last known-good state.
        ESP_LOGW(TAG, "TCA9555 input-port read failed: %s", esp_err_to_name(err));
        return false;
    }

    // 1->0 (unpressed->pressed, active-low) transition on each bit, checked
    // in KEY1/KEY2/KEY3 order -- if more than one button transitions in the
    // same poll interval, only the first is reported this cycle (a rare
    // simultaneous-multi-press case, not worth extra state to handle for a
    // single-button-at-a-time volume control).
    bool prev_key1 = (s_prev_state & KEY1_BIT) != 0, now_key1 = (state & KEY1_BIT) != 0;
    bool prev_key2 = (s_prev_state & KEY2_BIT) != 0, now_key2 = (state & KEY2_BIT) != 0;
    bool prev_key3 = (s_prev_state & KEY3_BIT) != 0, now_key3 = (state & KEY3_BIT) != 0;
    s_prev_state = state;

    if (prev_key1 && !now_key1) {
        *out_button = BUTTON_KEY1;
        return true;
    }
    if (prev_key2 && !now_key2) {
        *out_button = BUTTON_KEY2;
        return true;
    }
    if (prev_key3 && !now_key3) {
        *out_button = BUTTON_KEY3;
        return true;
    }
    return false;
}
