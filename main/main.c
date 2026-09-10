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
#include "wake_word.h"

static const char *TAG = "haro";

void app_main(void)
{
    ESP_LOGI(TAG, "Haro firmware starting");

    // TEMPORARY manual smoke test for Tasks 4-5 (audio_pipeline, wake_word).
    // Removed by Task 10, which replaces main.c wholesale. Hardware
    // verification (flash + physically saying "Hi ESP" near the board) is
    // deferred until the physical board arrives; this task's acceptance bar
    // is a clean `idf.py build` for esp32s3 only.
    ESP_ERROR_CHECK(audio_pipeline_init());
    ESP_LOGI(TAG, "audio_pipeline initialized");

    QueueHandle_t wake_queue = xQueueCreate(4, sizeof(wake_word_event_type_t));
    ESP_ERROR_CHECK(wake_word_start(wake_queue));
    ESP_LOGI(TAG, "wake_word started");

    wake_word_event_type_t evt;
    while (true) {
        if (xQueueReceive(wake_queue, &evt, portMAX_DELAY)) {
            ESP_LOGI(TAG, "wake word detected!");
        }
    }
}
#endif
