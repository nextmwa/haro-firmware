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
 *
 * Multi-network memory (added after real-hardware use surfaced the gap):
 * network_prov_mgr / esp_wifi's own credential storage holds exactly ONE
 * network. Haro moves between at least two known locations in practice
 * (home, office) -- with a single slot, arriving somewhere it was
 * previously provisioned for *before* means the new provisioning
 * overwrites the old one, and arriving back at the old location leaves it
 * stuck retrying a now-out-of-range SSID forever (confirmed on real
 * hardware: continuous "WiFi disconnected, reconnecting" with no
 * successful association -- there is no automatic fallback to SoftAP once
 * already provisioned, by design, see the not-provisioned/provisioned
 * branch below). wifi_profiles.c adds a small remembered-networks list
 * (up to WIFI_PROFILES_MAX, persisted in NVS under this file's own
 * "haro_wifi"/"profiles" key -- deliberately separate from
 * network_prov_mgr's own NVS storage) so a location Haro has already been
 * provisioned for once is recognized automatically from then on: on boot,
 * scan for visible networks, and if any remembered one is in range,
 * connect to it directly without needing to re-provision. Only when NONE
 * of the remembered networks are visible (a genuinely new location, or the
 * very first boot) does SoftAP provisioning start, exactly as before --
 * and whatever credentials are accepted there get remembered for next time
 * too.
 */

#include "haro_wifi_provisioning.h"
#include "wifi_profiles.h"
#include "face_display.h"
#include "haro_config.h"

#include "esp_console.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_softap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// SSID/password match the Pi's captive-portal `hotspot_password` config
// default ("haro1234") for behavioral parity where it still applies. The
// same string doubles as the protocomm security-1 proof-of-possession, so
// the provisioning app/CLI only needs to prompt for one shared secret.
#define WIFI_PROV_AP_SSID "Haro-Setup"
#define WIFI_PROV_AP_PASS "haro1234"
#define WIFI_PROV_POP     "haro1234"

// How long to wait for a connect attempt to a remembered network to
// succeed before giving up on it and falling back to SoftAP provisioning
// (e.g. the AP's password changed since it was last remembered, or it
// dropped off the air between the scan and the connect attempt).
#define KNOWN_NETWORK_CONNECT_TIMEOUT_MS 15000

#define NVS_NAMESPACE "haro_wifi"
#define NVS_KEY_PROFILES "profiles"

static const char *TAG = "wifi_provisioning";

// Set once WiFi actually has an IP (either the remembered-network path
// below, or SoftAP provisioning's own internal connect -- see
// NETWORK_PROV_END's comment). Either event means "WiFi is up".
#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_event_group;
static wifi_profile_store_t s_store;

// Stashed from NETWORK_PROV_WIFI_CRED_RECV (event data is a
// `wifi_sta_config_t*`, confirmed against
// managed_components/espressif__network_provisioning/include/network_provisioning/manager.h's
// NETWORK_PROV_WIFI_CRED_RECV doc comment) so NETWORK_PROV_WIFI_CRED_SUCCESS
// can remember this network for next time -- only once we know these
// credentials actually worked, not merely that they were received.
static char s_pending_ssid[WIFI_PROFILE_SSID_LEN + 1];
static char s_pending_password[WIFI_PROFILE_PASSWORD_LEN + 1];

static void save_profile_store(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not open NVS to save remembered networks: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(handle, NVS_KEY_PROFILES, &s_store, sizeof(s_store));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not save remembered networks: %s", esp_err_to_name(err));
    }
    nvs_close(handle);
}

static void load_profile_store(void)
{
    memset(&s_store, 0, sizeof(s_store));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not open NVS to load remembered networks: %s", esp_err_to_name(err));
        return;
    }

    size_t length = sizeof(s_store);
    err = nvs_get_blob(handle, NVS_KEY_PROFILES, &s_store, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no remembered networks yet");
    } else if (err != ESP_OK || length != sizeof(s_store)) {
        // A size mismatch means an on-flash layout from a different build
        // of wifi_profile_store_t (e.g. after WIFI_PROFILES_MAX changes) --
        // treat as absent rather than reading it misaligned.
        ESP_LOGW(TAG, "remembered-networks data unreadable (%s), starting empty", esp_err_to_name(err));
        memset(&s_store, 0, sizeof(s_store));
    } else {
        ESP_LOGI(TAG, "loaded %u remembered network(s)", s_store.count);
    }
    nvs_close(handle);
}

static int find_profile_by_ssid(const char *ssid)
{
    for (uint8_t i = 0; i < s_store.count; i++) {
        if (strncmp(s_store.profiles[i].ssid, ssid, WIFI_PROFILE_SSID_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

// If this network has a remembered server_url override (set via the
// "set_server_url" console command below), apply it to haro_config now so
// haro_config_get_server_url() -- read once, later in main.c's app_main()
// -- returns the address that's actually reachable from THIS network
// rather than whatever the last-connected network left behind. A profile
// with no override (server_url[0] == '\0', the default for every network
// until someone runs the console command) leaves haro_config's existing
// value alone, falling through to its own compile-time default.
static void apply_profile_server_url(const wifi_profile_t *profile)
{
    if (profile->server_url[0] == '\0') {
        return;
    }
    esp_err_t err = haro_config_set_server_url(profile->server_url);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not apply remembered server_url \"%s\": %s", profile->server_url, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "using remembered server_url \"%s\" for \"%s\"", profile->server_url, profile->ssid);
    }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
        case NETWORK_PROV_START:
            ESP_LOGI(TAG, "Provisioning started: join \"%s\" and provision with the "
                     "Espressif provisioning app or esp_prov.py", WIFI_PROV_AP_SSID);
            break;
        case NETWORK_PROV_WIFI_CRED_RECV: {
            const wifi_sta_config_t *sta = (const wifi_sta_config_t *)event_data;
            // .ssid/.password are fixed-size byte arrays, not guaranteed
            // null-terminated at their max length -- see wifi_sta_config_t's
            // doc comment in esp_wifi_types_generic.h.
            strncpy(s_pending_ssid, (const char *)sta->ssid, WIFI_PROFILE_SSID_LEN);
            s_pending_ssid[WIFI_PROFILE_SSID_LEN] = '\0';
            strncpy(s_pending_password, (const char *)sta->password, WIFI_PROFILE_PASSWORD_LEN);
            s_pending_password[WIFI_PROFILE_PASSWORD_LEN] = '\0';
            ESP_LOGI(TAG, "Received WiFi credentials for \"%s\"", s_pending_ssid);
            break;
        }
        case NETWORK_PROV_WIFI_CRED_FAIL:
            ESP_LOGE(TAG, "Provisioning failed, resetting state machine to allow retry");
            network_prov_mgr_reset_wifi_sm_state_on_failure();
            break;
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            ESP_LOGI(TAG, "WiFi credentials accepted, remembering \"%s\" for next time", s_pending_ssid);
            wifi_profiles_upsert(&s_store, s_pending_ssid, s_pending_password);
            save_profile_store();
            break;
        case NETWORK_PROV_END: {
            ESP_LOGI(TAG, "Provisioning service stopped");
            network_prov_mgr_deinit();
            // Register the same WIFI_EVENT handler try_connect_to_a_
            // remembered_network() registers on its own path (:332) --
            // found by inspection that this fresh-provisioning path never
            // did, leaving WIFI_EVENT_STA_DISCONNECTED with no handler to
            // call esp_wifi_connect() from. A network drop or AP reboot
            // any time after a first-time provisioning left Haro offline
            // until a manual power cycle (which takes the remembered-
            // network path instead, where the handler IS registered).
            // Espressif's own reference example for this component
            // (managed_components/espressif__network_provisioning/examples/
            // wifi_prov/main/app_main.c) registers its WIFI_EVENT handler
            // exactly here, right after network_prov_mgr_deinit() -- this
            // file's header comment on Task 9 already flagged that
            // reference but this specific line was never carried over.
            ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
            // Reached only once the manager's own internal state machine
            // has already connected the station (see this file's header
            // comment on NETWORK_PROV_END) -- safe to show the same "we're
            // online" confirmation the remembered-network path shows.
            face_display_show_wifi_connected();
            // A brand-new network has no remembered server_url yet (there's
            // no way to enter one through the provisioning app) -- this is
            // a no-op the first time, and picks up whatever was set on a
            // previous visit to this same network otherwise.
            int idx = find_profile_by_ssid(s_pending_ssid);
            if (idx >= 0) {
                apply_profile_server_url(&s_store.profiles[idx]);
            }
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            break;
        }
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

// Scans for currently visible networks and, if any of them is one Haro
// already knows, connects to it directly (no provisioning UI involved).
// Returns true if that connect succeeded within
// KNOWN_NETWORK_CONNECT_TIMEOUT_MS, false if no remembered network was in
// range or the connect attempt didn't pan out (either way, the caller
// falls back to SoftAP provisioning).
static bool try_connect_to_a_remembered_network(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Deliberately NOT registering the WIFI_EVENT handler yet: its
    // WIFI_EVENT_STA_START case would auto-connect using whatever STA
    // config happens to already be set (stale from a previous boot, or
    // empty) the moment esp_wifi_start() above fires that event --
    // racing this function's own scan. It's registered below only once a
    // specific remembered network has actually been chosen.
    ESP_LOGI(TAG, "Scanning for %u remembered network(s)...", s_store.count);
    face_display_show_wifi_searching();
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(esp_wifi_stop());
        return false;
    }

    uint16_t ap_count = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
    if (ap_count == 0) {
        ESP_LOGI(TAG, "No networks visible");
        // This and the "none in range" return below are the NORMAL case
        // at a genuinely new location -- the exact moment provisioning
        // must start -- so this esp_wifi_stop() isn't a rare cleanup path,
        // it's the common one. Found by inspection that only the failed-
        // connect-attempt return further down called it, leaving STA
        // started (against network_prov_mgr_start_provisioning()'s own
        // documented precondition, see its call site's comment) on every
        // other return false here.
        ESP_ERROR_CHECK(esp_wifi_stop());
        return false;
    }

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    wifi_scan_result_t *scan_results = calloc(ap_count, sizeof(wifi_scan_result_t));
    if (ap_records == NULL || scan_results == NULL) {
        ESP_LOGE(TAG, "OOM allocating scan result buffers");
        free(ap_records);
        free(scan_results);
        ESP_ERROR_CHECK(esp_wifi_stop());
        return false;
    }

    uint16_t got = ap_count;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&got, ap_records));
    for (uint16_t i = 0; i < got; i++) {
        strncpy(scan_results[i].ssid, (const char *)ap_records[i].ssid, WIFI_PROFILE_SSID_LEN);
        scan_results[i].ssid[WIFI_PROFILE_SSID_LEN] = '\0';
        scan_results[i].rssi = ap_records[i].rssi;
    }
    free(ap_records);

    int match = wifi_profiles_find_best_match(&s_store, scan_results, got);
    free(scan_results);

    if (match < 0) {
        ESP_LOGI(TAG, "None of the remembered networks are in range");
        ESP_ERROR_CHECK(esp_wifi_stop());
        return false;
    }

    const wifi_profile_t *profile = &s_store.profiles[match];
    ESP_LOGI(TAG, "Found remembered network \"%s\", connecting...", profile->ssid);

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, profile->ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, profile->password, sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                            pdMS_TO_TICKS(KNOWN_NETWORK_CONNECT_TIMEOUT_MS));
    if (bits & WIFI_CONNECTED_BIT) {
        face_display_show_wifi_connected();
        apply_profile_server_url(profile);
        return true;
    }

    ESP_LOGW(TAG, "\"%s\" did not connect within %ds, falling back to provisioning",
             profile->ssid, KNOWN_NETWORK_CONNECT_TIMEOUT_MS / 1000);
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler));
    esp_wifi_disconnect();
    // Reset to the state network_prov_mgr_start_provisioning() expects
    // below (it calls esp_wifi_set_mode()+esp_wifi_start() itself and was
    // never designed to be called on top of an already-started STA).
    ESP_ERROR_CHECK(esp_wifi_stop());
    return false;
}

// --- "set_server_url" serial console command ----------------------------
//
// The WiFi side of "same problem, different value" (see this file's header
// comment on multi-network memory) has an app to drive it; this one
// doesn't -- the Espressif provisioning app only carries WiFi credentials,
// nothing app-specific. A serial console reachable over the same
// USB/UART port already used for `idf.py monitor` is the simplest channel
// that needs no new hardware, no new protocol, and no phone app update:
// connect Haro to a network (remembered fast-path or fresh provisioning,
// either one), open a serial terminal, run `set_server_url <url>` once,
// done -- remembered for that network from then on.
static int cmd_set_server_url(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: set_server_url <ws://host:port>\n");
        return 1;
    }

    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    if (err != ESP_OK) {
        printf("not connected to a network right now: %s\n", esp_err_to_name(err));
        return 1;
    }

    char ssid[WIFI_PROFILE_SSID_LEN + 1];
    strncpy(ssid, (const char *)ap_info.ssid, WIFI_PROFILE_SSID_LEN);
    ssid[WIFI_PROFILE_SSID_LEN] = '\0';

    int idx = find_profile_by_ssid(ssid);
    if (idx < 0) {
        // Shouldn't happen -- every network reachable via
        // wifi_provisioning_ensure_connected() (remembered or freshly
        // provisioned) is upserted into s_store before WiFi ever comes up.
        printf("\"%s\" is not a remembered network\n", ssid);
        return 1;
    }

    strncpy(s_store.profiles[idx].server_url, argv[1], WIFI_PROFILE_SERVER_URL_LEN);
    s_store.profiles[idx].server_url[WIFI_PROFILE_SERVER_URL_LEN] = '\0';
    save_profile_store();
    haro_config_set_server_url(argv[1]);

    printf("server URL for \"%s\" set to \"%s\" -- reboot Haro for it to take effect\n", ssid, argv[1]);
    return 0;
}

// set_server_url above only ever touches the CURRENTLY connected
// network's profile (esp_wifi_sta_get_ap_info()) -- no way to fix a
// different remembered network's stale server_url without physically
// being on that network first. Found in real use: the whole point of a
// serial console reachable over USB is that it doesn't care which WiFi
// network Haro is on, but this command still did. set_server_url_for
// takes the SSID explicitly instead of inferring it, so any remembered
// network's server_url can be corrected from wherever Haro's USB cable
// happens to be plugged in right now. Does NOT touch haro_config's live
// server_url (unlike set_server_url above) -- that value is meant to
// reflect the network Haro is ACTUALLY connected to right now, which this
// command makes no claim about; the correction only takes effect the next
// time Haro connects to the named network (apply_profile_server_url()).
static int cmd_set_server_url_for(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: set_server_url_for <ssid> <ws://host:port>\n");
        return 1;
    }

    int idx = find_profile_by_ssid(argv[1]);
    if (idx < 0) {
        printf("\"%s\" is not a remembered network\n", argv[1]);
        return 1;
    }

    strncpy(s_store.profiles[idx].server_url, argv[2], WIFI_PROFILE_SERVER_URL_LEN);
    s_store.profiles[idx].server_url[WIFI_PROFILE_SERVER_URL_LEN] = '\0';
    save_profile_store();

    printf("server URL for \"%s\" set to \"%s\" -- takes effect next time Haro connects to that network\n",
           argv[1], argv[2]);
    return 0;
}

static void start_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "haro>";

    esp_err_t err = esp_console_new_repl_stdio(&repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not start serial console: %s -- set_server_url unavailable this boot",
                 esp_err_to_name(err));
        return;
    }

    esp_console_register_help_command();
    const esp_console_cmd_t cmd = {
        .command = "set_server_url",
        .help = "Remember the haro-server address for the currently connected WiFi network",
        .hint = "<ws://host:port>",
        .func = &cmd_set_server_url,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    const esp_console_cmd_t cmd_for = {
        .command = "set_server_url_for",
        .help = "Remember the haro-server address for a named remembered WiFi network "
                "(unlike set_server_url, works no matter which network Haro is on right now)",
        .hint = "<ssid> <ws://host:port>",
        .func = &cmd_set_server_url_for,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd_for));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

static void start_provisioning_and_wait(void)
{
    ESP_LOGI(TAG, "Starting SoftAP provisioning service");
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_softap,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(config));
    ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(
        NETWORK_PROV_SECURITY_1, WIFI_PROV_POP, WIFI_PROV_AP_SSID, WIFI_PROV_AP_PASS));
    // "Waiting for someone to provision me" -- see face_display.h's comment
    // on face_display_show_wifi_connected() for why this reuses the normal
    // mood system (EXPR_SETUP) instead of a one-shot icon like the other
    // two WiFi status moments.
    face_display_show(EXPR_SETUP);

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
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
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    load_profile_store();

    if (s_store.count == 0 || !try_connect_to_a_remembered_network()) {
        // Deliberately NOT registering our own unconditional WIFI_EVENT
        // handler before this point (beyond the temporary one
        // try_connect_to_a_remembered_network() already cleaned up on its
        // own failure path): while provisioning is active,
        // network_prov_mgr_init() registers its own internal WIFI_EVENT
        // handler (managed_components/espressif__network_provisioning/src/manager.c:478)
        // that owns the credential-connect/retry/give-up state machine.
        // Espressif's own reference example
        // (examples/wifi_prov/main/app_main.c) registers the app-level
        // WIFI_EVENT handler ONLY after network_prov_mgr_deinit() -- never
        // while the manager is active. We follow that exactly.
        //
        // start_provisioning_and_wait() blocks (portMAX_DELAY) until
        // WIFI_CONNECTED_BIT is set, same guarantee
        // try_connect_to_a_remembered_network() already gave on its own
        // success path above -- either way, WiFi is up by the time this
        // function returns.
        start_provisioning_and_wait();
    }

    start_console();

    return ESP_OK;
}
