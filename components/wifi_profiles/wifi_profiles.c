#include "wifi_profiles.h"
#include <string.h>

void wifi_profiles_upsert(wifi_profile_store_t *store, const char *ssid, const char *password)
{
    for (uint8_t i = 0; i < store->count; i++) {
        if (strncmp(store->profiles[i].ssid, ssid, WIFI_PROFILE_SSID_LEN) == 0) {
            strncpy(store->profiles[i].password, password, WIFI_PROFILE_PASSWORD_LEN);
            store->profiles[i].password[WIFI_PROFILE_PASSWORD_LEN] = '\0';
            return;
        }
    }

    if (store->count >= WIFI_PROFILES_MAX) {
        // Oldest (index 0) forgotten to make room -- see this function's
        // header comment.
        memmove(&store->profiles[0], &store->profiles[1], sizeof(wifi_profile_t) * (WIFI_PROFILES_MAX - 1));
        store->count = WIFI_PROFILES_MAX - 1;
    }

    // Zeroed first, not just overwritten field-by-field: after an eviction
    // shift above, this slot (the new last one) can still hold a stale
    // copy of what used to be one slot to its right (memmove doesn't clear
    // the vacated tail) -- server_url in particular has nothing below that
    // would otherwise overwrite it.
    wifi_profile_t *slot = &store->profiles[store->count];
    memset(slot, 0, sizeof(*slot));
    strncpy(slot->ssid, ssid, WIFI_PROFILE_SSID_LEN);
    strncpy(slot->password, password, WIFI_PROFILE_PASSWORD_LEN);
    store->count++;
}

int wifi_profiles_find_best_match(const wifi_profile_store_t *store,
                                   const wifi_scan_result_t *results, uint16_t result_count)
{
    int best_index = -1;
    int best_rssi = INT32_MIN;

    for (uint16_t r = 0; r < result_count; r++) {
        for (uint8_t p = 0; p < store->count; p++) {
            if (strncmp(store->profiles[p].ssid, results[r].ssid, WIFI_PROFILE_SSID_LEN) != 0) {
                continue;
            }
            if (best_index == -1 || results[r].rssi > best_rssi) {
                best_index = p;
                best_rssi = results[r].rssi;
            }
        }
    }

    return best_index;
}
