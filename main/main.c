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
static const char *TAG = "haro";

void app_main(void)
{
    ESP_LOGI(TAG, "Haro firmware starting");
}
#endif
