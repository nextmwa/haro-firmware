#include "audio_pipeline.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

static const char *TAG = "audio_pipeline";

#define GPIO_I2C_SCL GPIO_NUM_10
#define GPIO_I2C_SDA GPIO_NUM_11
#define GPIO_I2S_MCLK GPIO_NUM_12
#define GPIO_I2S_BCLK GPIO_NUM_13
#define GPIO_I2S_WS   GPIO_NUM_14
#define GPIO_I2S_DIN  GPIO_NUM_15  // mic data in
#define GPIO_I2S_DOUT GPIO_NUM_16  // speaker data out

static i2c_master_bus_handle_t s_i2c_bus;
static i2s_chan_handle_t s_tx_handle;
static i2s_chan_handle_t s_rx_handle;
static esp_codec_dev_handle_t s_record_dev;
static esp_codec_dev_handle_t s_play_dev;

// Last volume applied via audio_pipeline_set_volume_percent() (or the
// init-time default, 80, before that's ever been called) -- see that
// function's header comment for why this needs to be remembered rather
// than re-applying a fixed literal every time audio_pipeline_stop_
// playback() reopens the codec.
static int s_volume_percent = 80;

static esp_err_t init_i2c(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_I2C_SDA,
        .scl_io_num = GPIO_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
    };
    return i2c_new_master_bus(&bus_config, &s_i2c_bus);
}

// Found on real hardware: total silence on the speaker regardless of what's
// written to it. Root cause per the board's official schematic/pinout: the
// NS4150B power amplifier's enable line (PA_EN) is not a direct MCU GPIO --
// it's wired to EXIO8, a pin on the onboard TCA9555 I2C GPIO expander (the
// same "Extend_IO0..15" numbering the pinout diagram uses for the expander,
// with EXIO0-7 = TCA9555 port 0 bits 0-7 and EXIO8-15 = port 1 bits 0-7, so
// EXIO8 = port 1 bit 0). es8311_codec_cfg_t's `pa_pin = -1` above is
// unrelated and still correct -- that field is esp_codec_dev's own
// direct-GPIO amp-enable convenience, which doesn't apply here since PA_EN
// isn't a direct GPIO at all. Nothing in this codebase (or, per the
// existing comment on `pa_pin`, Waveshare's own demo) has ever driven
// EXIO8, so the amplifier has been sitting disabled this whole time: the
// ES8311 DAC can produce a perfectly correct analog signal and it never
// reaches the physical speaker.
//
// TCA9555 register map (standard, NXP/TI TCA9555 datasheet, not board-
// specific): Input Port 0/1 = 0x00/0x01, Output Port 0/1 = 0x02/0x03,
// Polarity Inversion 0/1 = 0x04/0x05, Configuration 0/1 = 0x06/0x07 (0 =
// output, 1 = input, all pins default to input on power-up). Address 0x20
// is inferred from the schematic's address-pin strapping (A0/A1/A2 tied to
// GND via 0R jumpers), the standard TCA9555 default -- not read off an
// explicit address label, so this is probed, not assumed blind.
#define TCA9555_I2C_ADDR 0x20
#define TCA9555_REG_CONFIG_PORT1 0x07
#define TCA9555_REG_OUTPUT_PORT1 0x03
#define TCA9555_PA_EN_BIT (1 << 0) // EXIO8 = port 1, bit 0

static esp_err_t tca9555_enable_pa(void)
{
    esp_err_t err = i2c_master_probe(s_i2c_bus, TCA9555_I2C_ADDR, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TCA9555 not found at 0x%02X (%s) -- speaker amplifier may stay disabled",
                 TCA9555_I2C_ADDR, esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9555_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev;
    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        return err;
    }

    // Read-modify-write both registers: only touch the PA_EN bit, leave
    // every other port-1 pin (KEY1-3 buttons, Extend_IO9-15) exactly as it
    // was, since this component has no business deciding their direction
    // or level.
    uint8_t reg = TCA9555_REG_CONFIG_PORT1;
    uint8_t val = 0;
    err = i2c_master_transmit_receive(dev, &reg, 1, &val, 1, -1);
    if (err == ESP_OK) {
        uint8_t write_buf[2] = { TCA9555_REG_CONFIG_PORT1, (uint8_t)(val & ~TCA9555_PA_EN_BIT) };
        err = i2c_master_transmit(dev, write_buf, sizeof(write_buf), -1);
    }
    if (err == ESP_OK) {
        reg = TCA9555_REG_OUTPUT_PORT1;
        err = i2c_master_transmit_receive(dev, &reg, 1, &val, 1, -1);
    }
    if (err == ESP_OK) {
        uint8_t write_buf[2] = { TCA9555_REG_OUTPUT_PORT1, (uint8_t)(val | TCA9555_PA_EN_BIT) };
        err = i2c_master_transmit(dev, write_buf, sizeof(write_buf), -1);
    }

    i2c_master_bus_rm_device(dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TCA9555 PA_EN write failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Speaker amplifier enabled (TCA9555 EXIO8)");
    }
    return err;
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle);
    if (err != ESP_OK) return err;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(32, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_I2S_MCLK,
            .bclk = GPIO_I2S_BCLK,
            .ws   = GPIO_I2S_WS,
            .dout = GPIO_I2S_DOUT,
            .din  = GPIO_I2S_DIN,
        },
    };
    err = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (err != ESP_OK) return err;
    err = i2s_channel_init_std_mode(s_rx_handle, &std_cfg);
    if (err != ESP_OK) return err;
    err = i2s_channel_enable(s_tx_handle);
    if (err != ESP_OK) return err;
    return i2s_channel_enable(s_rx_handle);
}

static esp_err_t init_mic_codec(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_1, .rx_handle = s_rx_handle, .tx_handle = NULL };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = { .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c_bus };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = ctrl_if,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *codec_if = es7210_codec_new(&es7210_cfg);

    esp_codec_dev_cfg_t dev_cfg = { .codec_if = codec_if, .data_if = data_if, .dev_type = ESP_CODEC_DEV_TYPE_IN };
    s_record_dev = esp_codec_dev_new(&dev_cfg);
    if (s_record_dev == NULL) return ESP_FAIL;

    esp_codec_dev_sample_info_t fs = { .sample_rate = 16000, .channel = 2, .bits_per_sample = 32 };
    esp_err_t err = esp_codec_dev_open(s_record_dev, &fs);
    if (err != ESP_OK) return err;

    // es7210_open() defaults to 30dB PGA gain
    // (managed_components/espressif__esp_codec_dev/device/es7210/es7210.c),
    // which measured as far too quiet on this board (peak sample amplitude
    // ~100-180 of a possible 32767 while speaking normally at ~20-30cm) --
    // wake word never triggered until this was raised. 37.5dB is the ES7210
    // PGA's maximum (get_db() in es7210.c caps there); confirmed on real
    // hardware to fix wake-word detection.
    return esp_codec_dev_set_in_gain(s_record_dev, 37.5f);
}

static esp_err_t init_speaker_codec(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_1, .rx_handle = NULL, .tx_handle = s_tx_handle };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = { .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c_bus };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es8311_cfg = {
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .pa_pin = -1,        // no dedicated PA-enable GPIO on this board's ES8311 path (verified: Waveshare's own demo leaves this unmanaged)
        .use_mclk = false,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);

    esp_codec_dev_cfg_t dev_cfg = { .codec_if = codec_if, .data_if = data_if, .dev_type = ESP_CODEC_DEV_TYPE_OUT };
    s_play_dev = esp_codec_dev_new(&dev_cfg);
    if (s_play_dev == NULL) return ESP_FAIL;

    esp_codec_dev_sample_info_t fs = { .sample_rate = 16000, .channel = 1, .bits_per_sample = 16 };
    // Found on real hardware (once TCA9555_PA_EN was fixed and sound was
    // actually audible for the first time): 60 was quiet even close up, but
    // 100 (max) was too loud. esp_codec_dev_set_out_vol()'s `volume` is
    // 0-100, mapped internally to the ES8311's dB gain curve. s_volume_
    // percent starts at that same 80 default (see its own declaration) --
    // main.c overrides it right after audio_pipeline_init() returns, once
    // it's read the persisted volume level from NVS.
    esp_codec_dev_set_out_vol(s_play_dev, s_volume_percent);
    return esp_codec_dev_open(s_play_dev, &fs);
}

esp_err_t audio_pipeline_init(void)
{
    esp_err_t err = init_i2c();
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2c init failed: %s", esp_err_to_name(err)); return err; }
    err = init_i2s();
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2s init failed: %s", esp_err_to_name(err)); return err; }
    err = init_mic_codec();
    if (err != ESP_OK) { ESP_LOGE(TAG, "mic codec init failed: %s", esp_err_to_name(err)); return err; }
    err = init_speaker_codec();
    if (err != ESP_OK) { ESP_LOGE(TAG, "speaker codec init failed: %s", esp_err_to_name(err)); return err; }
    // Non-fatal: a failure here means no sound reaches the physical
    // speaker, not a broken device -- mic capture, wake word, and the rest
    // of the pipeline are unaffected. tca9555_enable_pa() already logs the
    // specifics.
    tca9555_enable_pa();
    return ESP_OK;
}

esp_err_t audio_pipeline_get_i2c_bus(i2c_master_bus_handle_t *out_bus)
{
    if (out_bus == NULL || s_i2c_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_bus = s_i2c_bus;
    return ESP_OK;
}

esp_err_t audio_pipeline_read(void *buf, size_t len, size_t *bytes_read)
{
    esp_err_t err = esp_codec_dev_read(s_record_dev, buf, len);
    if (bytes_read) *bytes_read = (err == ESP_OK) ? len : 0;
    return err;
}

esp_err_t audio_pipeline_write(const void *buf, size_t len)
{
    return esp_codec_dev_write(s_play_dev, (void *)buf, len);
}

esp_err_t audio_pipeline_stop_playback(void)
{
    esp_codec_dev_close(s_play_dev);
    // Same fs as init_speaker_codec()'s original open. Volume: re-applies
    // s_volume_percent (the last value set via audio_pipeline_set_volume_
    // percent(), not a hardcoded literal) -- esp_codec_dev_close()/open()
    // doesn't remember volume across the cycle on its own, and this
    // function runs after every spoken reply, so a hardcoded value here
    // would silently undo any runtime volume change on the very next turn.
    esp_codec_dev_sample_info_t fs = { .sample_rate = 16000, .channel = 1, .bits_per_sample = 16 };
    esp_codec_dev_set_out_vol(s_play_dev, s_volume_percent);
    return esp_codec_dev_open(s_play_dev, &fs);
}

esp_err_t audio_pipeline_set_volume_percent(int percent)
{
    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    s_volume_percent = percent;
    if (s_play_dev == NULL) {
        // Not yet initialized -- s_volume_percent is still recorded and
        // will be applied by init_speaker_codec()'s own esp_codec_dev_
        // set_out_vol() call once audio_pipeline_init() runs. Not an
        // error: main.c can legitimately call this before audio_pipeline_
        // init() only if it reads NVS before initializing audio, which
        // isn't the current boot order, but there's no reason to force
        // callers to know that.
        return ESP_OK;
    }
    return esp_codec_dev_set_out_vol(s_play_dev, percent);
}
