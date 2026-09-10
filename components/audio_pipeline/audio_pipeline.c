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
    return esp_codec_dev_open(s_record_dev, &fs);
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
    esp_codec_dev_set_out_vol(s_play_dev, 60);
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
    return init_speaker_codec();
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

esp_err_t audio_pipeline_set_out_volume(int volume)
{
    return esp_codec_dev_set_out_vol(s_play_dev, volume);
}
