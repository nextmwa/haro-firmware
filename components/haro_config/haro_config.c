#include "haro_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#include <string.h>

#define NVS_NAMESPACE "haro"
#define KEY_SERVER_URL "server_url"

esp_err_t haro_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t haro_config_get_server_url(char *out, size_t out_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t len = out_len;
        err = nvs_get_str(handle, KEY_SERVER_URL, out, &len);
        nvs_close(handle);
    }

    // Factory-fresh NVS has neither the "haro" namespace nor the
    // "server_url" key written yet -- nvs_open(NVS_READONLY) returns
    // ESP_ERR_NVS_NOT_FOUND when the namespace itself doesn't exist, and
    // nvs_get_str() returns the same code when the namespace exists but the
    // key hasn't been set. haro_config_set_server_url() is currently called
    // from nowhere in the boot path (no provisioning flow writes
    // server_url), so this is the expected first-boot state, not an error
    // condition -- fall back to the compile-time Kconfig default instead of
    // returning failure to the caller. See components/haro_config/Kconfig
    // for why this exists and how to override it.
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        size_t needed = strlen(CONFIG_HARO_DEFAULT_SERVER_URL) + 1;
        if (needed > out_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out, CONFIG_HARO_DEFAULT_SERVER_URL, needed);
        return ESP_OK;
    }

    return err;
}

esp_err_t haro_config_set_server_url(const char *url)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, KEY_SERVER_URL, url);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
