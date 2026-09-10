#include "sdkconfig.h"
#include "esp_log.h"

#if CONFIG_IDF_TARGET_LINUX
#include "unity.h"

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
#else
#include "audio_pipeline.h"

static const char *TAG = "haro";

void app_main(void)
{
    ESP_LOGI(TAG, "Haro firmware starting");

    // TEMPORARY manual smoke test for Task 4 (audio_pipeline). Removed by
    // Task 10, which replaces main.c wholesale. Hardware verification
    // (flash + physical mic/speaker loopback) is deferred until the
    // physical board arrives; this task's acceptance bar is a clean
    // `idf.py build` for esp32s3 only.
    ESP_ERROR_CHECK(audio_pipeline_init());
    ESP_LOGI(TAG, "audio_pipeline initialized");
}
#endif
