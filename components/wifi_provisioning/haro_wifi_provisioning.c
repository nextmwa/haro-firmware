/*
 * Task 9 UX-question finding (full investigation + source quotes in
 * .superpowers/sdd/2026-09-10-esp32-s3-firmware/task-9-report.md):
 *
 * `network_prov_scheme_softap` does NOT serve a plain browser-fillable HTML
 * form. Its `prov_start()` (managed_components/espressif__network_provisioning/
 * src/scheme_softap.c) only calls `protocomm_httpd_start()`, and protocomm's
 * HTTP transport (esp-idf/components/protocomm/src/transports/protocomm_httpd.c)
 * registers exclusively HTTP_POST endpoints ("prov-session", "prov-config",
 * "prov-scan", "prov-ctrl") whose request/response bodies are raw
 * protobuf-encoded messages, gated behind a protocomm security handshake
 * (X25519 key exchange + proof-of-possession for NETWORK_PROV_SECURITY_1).
 * There is no GET route and no HTML anywhere in this component. Espressif's
 * own reference example for this exact component
 * (managed_components/espressif__network_provisioning/examples/wifi_prov/main/app_main.c)
 * confirms the intended client is the official companion mobile app (ESP
 * SoftAP/BLE Provisioning) or the `esp_prov.py` CLI tool, normally
 * bootstrapped by scanning a QR code -- not a browser form.
 *
 * This is a confirmed, deliberate UX change from the Pi's current
 * browser-form captive portal, approved by the project owner after
 * escalation: end users provision this board by joining the "Haro-Setup"
 * SoftAP and using the official Espressif provisioning app / esp_prov.py,
 * exactly as implemented below (no custom HTML form, no hybrid fallback).
 */

#include "haro_wifi_provisioning.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_softap.h"

// SSID/password match the Pi's captive-portal `hotspot_password` config
// default ("haro1234") for behavioral parity where it still applies. The
// same string doubles as the protocomm security-1 proof-of-possession, so
// the provisioning app/CLI only needs to prompt for one shared secret.
#define WIFI_PROV_AP_SSID "Haro-Setup"
#define WIFI_PROV_AP_PASS "haro1234"
#define WIFI_PROV_POP     "haro1234"

static const char *TAG = "wifi_provisioning";

// Set once WiFi actually has an IP (already-known-credentials path), or when
// the provisioning service reports NETWORK_PROV_END (SoftAP setup path,
// which by default only auto-stops after credentials are accepted and the
// station has connected). Either event means "WiFi is up".
#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;

static void event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
        case NETWORK_PROV_START:
            ESP_LOGI(TAG, "Provisioning started: join \"%s\" and provision with the "
                     "Espressif provisioning app or esp_prov.py", WIFI_PROV_AP_SSID);
            break;
        case NETWORK_PROV_WIFI_CRED_RECV:
            ESP_LOGI(TAG, "Received WiFi credentials");
            break;
        case NETWORK_PROV_WIFI_CRED_FAIL:
            ESP_LOGE(TAG, "Provisioning failed, resetting state machine to allow retry");
            network_prov_mgr_reset_wifi_sm_state_on_failure();
            break;
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            ESP_LOGI(TAG, "WiFi credentials accepted");
            break;
        case NETWORK_PROV_END:
            ESP_LOGI(TAG, "Provisioning service stopped");
            network_prov_mgr_deinit();
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            break;
        default:
            break;
        }
    } else if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi disconnected, reconnecting");
            esp_wifi_connect();
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t wifi_provisioning_ensure_connected(void)
{
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(config));

    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned) {
        ESP_LOGI(TAG, "Not provisioned, starting SoftAP provisioning service");
        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(
            NETWORK_PROV_SECURITY_1, WIFI_PROV_POP, WIFI_PROV_AP_SSID, WIFI_PROV_AP_PASS));
    } else {
        ESP_LOGI(TAG, "Already provisioned, connecting with known credentials");
        // network_prov_mgr_start_provisioning() always erases the RAM copy of
        // any existing credentials and forces SoftAP setup, even if already
        // provisioned (see network_prov_mgr_is_wifi_provisioned() doc in
        // network_provisioning/manager.h) -- so we must not call it here.
        // Free the manager and drive WiFi STA directly instead.
        ESP_ERROR_CHECK(network_prov_mgr_deinit());
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    return ESP_OK;
}
