#pragma once
#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// KEY1/KEY2/KEY3 -- the three onboard buttons on the TCA9555 I2C GPIO
// expander (same chip already used elsewhere in this project for the
// speaker amplifier enable and the camera PWDN/RESET lines), per the
// board's official pinout diagram: KEY1 = EXIO9, KEY2 = EXIO10,
// KEY3 = EXIO11 (all port 1, bits 1/2/3). RESET/BOOT are direct MCU pins
// (CHIP_PU/GPIO0), not on the expander, and are not application-usable
// buttons -- not represented here.
typedef enum {
    BUTTON_KEY1,
    BUTTON_KEY2,
    BUTTON_KEY3,
} button_id_t;

// Probes for the TCA9555 on `i2c_bus` (the shared bus from
// audio_pipeline_get_i2c_bus() -- this board has only one physical I2C bus,
// see audio_pipeline.c's own comment on why). Does not touch the expander's
// direction/config registers: KEY1-3's port-1 bits default to input on
// power-up and nothing else in this project's TCA9555 usage (PA_EN,
// camera PWDN/RESET) touches those specific bits, so they're already
// correctly configured as inputs by the time this runs.
esp_err_t buttons_init(i2c_master_bus_handle_t i2c_bus);

// Polls the TCA9555's input port once and reports at most one NEWLY
// pressed button (a 1->0 edge on that button's bit -- active-low, a button
// pulls its pin to GND when pressed) since the previous call. Returns true
// and sets *out_button if a press edge was seen, false otherwise (nothing
// new, or a read error -- treated as "no press" rather than propagated,
// since a transient I2C glitch here shouldn't be mistaken for a button
// event). No separate debounce timer: intended to be called periodically
// (e.g. every ~100ms) from a single task, and that polling interval is
// coarser than typical mechanical switch bounce (a few ms), so each poll
// already samples past any bounce. Not safe to call from more than one
// task concurrently (unsynchronized static edge-tracking state).
bool buttons_poll(button_id_t *out_button);

#ifdef __cplusplus
}
#endif
