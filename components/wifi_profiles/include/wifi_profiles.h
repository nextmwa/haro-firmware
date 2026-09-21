#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Pure logic for remembering multiple WiFi networks (home, office, ...),
// deliberately kept free of any ESP-IDF hardware dependency (no esp_wifi,
// no NVS) so it can run in the linux-target host tests
// (test/test_wifi_profiles.c), the same way protocol.c/orchestrator.c do.
// wifi_provisioning (haro_wifi_provisioning.c) owns the hardware glue:
// loading/saving a wifi_profile_store_t to NVS, running the actual WiFi
// scan, and calling into esp_wifi with whatever profile these functions
// pick.
//
// Why this exists: esp_wifi's own credential storage
// (esp_wifi_set_config/WIFI_STORAGE_FLASH) holds exactly one network. Real
// use here moves Haro between at least two known locations (home, office)
// -- with only one slot, arriving at the previously-unvisited location
// leaves the device stuck retrying a now-out-of-range SSID forever
// (confirmed on real hardware: continuous "WiFi disconnected, reconnecting"
// with no successful association). This adds a small remembered-networks
// list on top, so a location Haro has already been provisioned for once
// is recognized automatically from then on.

#define WIFI_PROFILES_MAX 5
#define WIFI_PROFILE_SSID_LEN 32
#define WIFI_PROFILE_PASSWORD_LEN 64
// Generous for a "ws://192.168.1.99:8765"-shaped URL, including a longer
// hostname in place of a raw IP (e.g. for an mDNS ".local" name).
#define WIFI_PROFILE_SERVER_URL_LEN 63

typedef struct {
    char ssid[WIFI_PROFILE_SSID_LEN + 1];
    char password[WIFI_PROFILE_PASSWORD_LEN + 1];
    // Empty string ("") means "no override remembered for this network --
    // use haro_config's compile-time default". Set via the
    // "set_server_url" serial console command (haro_wifi_provisioning.c)
    // once connected to this network, for the same reason wifi_profiles
    // itself exists: the haro-server host's LAN-local address differs
    // between locations (e.g. home vs office) even though it's the same
    // physical machine and the same port every time.
    char server_url[WIFI_PROFILE_SERVER_URL_LEN + 1];
} wifi_profile_t;

typedef struct {
    uint8_t count;
    wifi_profile_t profiles[WIFI_PROFILES_MAX];
} wifi_profile_store_t;

// One access point as seen in a WiFi scan just now -- trimmed to just what
// wifi_profiles_find_best_match() needs, not ESP-IDF's own
// wifi_ap_record_t, to keep this header hardware-independent.
typedef struct {
    char ssid[WIFI_PROFILE_SSID_LEN + 1];
    int8_t rssi;
} wifi_scan_result_t;

// Remembers ssid/password: updates the password in place if ssid is
// already remembered (leaving that profile's server_url untouched -- a
// re-provisioned password shouldn't wipe an already-set server_url
// override), otherwise appends a new profile with an empty server_url.
// When the store is already full (WIFI_PROFILES_MAX reached) and ssid is
// new, evicts the oldest entry (index 0, shifting the rest down) to make
// room -- the same "forget the least recently (re)provisioned network"
// behavior phones and laptops use for their own saved-network lists.
void wifi_profiles_upsert(wifi_profile_store_t *store, const char *ssid, const char *password);

// Matches `results` (access points visible right now, from a real WiFi
// scan) against store's remembered profiles and returns the index into
// store->profiles of the strongest (highest RSSI) remembered network
// that's actually in range, or -1 if none of the remembered networks are
// currently visible.
int wifi_profiles_find_best_match(const wifi_profile_store_t *store,
                                   const wifi_scan_result_t *results, uint16_t result_count);

#ifdef __cplusplus
}
#endif
