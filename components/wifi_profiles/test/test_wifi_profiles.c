#include "unity.h"
#include "wifi_profiles.h"
#include <string.h>

TEST_CASE("upsert adds a new profile to an empty store", "[wifi_profiles]")
{
    wifi_profile_store_t store = { 0 };
    wifi_profiles_upsert(&store, "WIFI_CASA", "casa-pass");
    TEST_ASSERT_EQUAL(1, store.count);
    TEST_ASSERT_EQUAL_STRING("WIFI_CASA", store.profiles[0].ssid);
    TEST_ASSERT_EQUAL_STRING("casa-pass", store.profiles[0].password);
}

TEST_CASE("upsert appends a second, different profile", "[wifi_profiles]")
{
    wifi_profile_store_t store = { 0 };
    wifi_profiles_upsert(&store, "WIFI_CASA", "casa-pass");
    wifi_profiles_upsert(&store, "2RStudio_interna", "ufficio-pass");
    TEST_ASSERT_EQUAL(2, store.count);
    TEST_ASSERT_EQUAL_STRING("WIFI_CASA", store.profiles[0].ssid);
    TEST_ASSERT_EQUAL_STRING("2RStudio_interna", store.profiles[1].ssid);
}

TEST_CASE("upsert updates the password in place for a known ssid", "[wifi_profiles]")
{
    wifi_profile_store_t store = { 0 };
    wifi_profiles_upsert(&store, "WIFI_CASA", "old-pass");
    wifi_profiles_upsert(&store, "WIFI_CASA", "new-pass");
    TEST_ASSERT_EQUAL(1, store.count);
    TEST_ASSERT_EQUAL_STRING("new-pass", store.profiles[0].password);
}

TEST_CASE("upsert leaves server_url empty for a newly added profile", "[wifi_profiles]")
{
    wifi_profile_store_t store = { 0 };
    wifi_profiles_upsert(&store, "WIFI_CASA", "casa-pass");
    TEST_ASSERT_EQUAL_STRING("", store.profiles[0].server_url);
}

TEST_CASE("upsert preserves an existing server_url when only the password changes", "[wifi_profiles]")
{
    wifi_profile_store_t store = { 0 };
    wifi_profiles_upsert(&store, "WIFI_CASA", "old-pass");
    strncpy(store.profiles[0].server_url, "ws://192.168.1.99:8765", WIFI_PROFILE_SERVER_URL_LEN);

    wifi_profiles_upsert(&store, "WIFI_CASA", "new-pass");

    TEST_ASSERT_EQUAL_STRING("new-pass", store.profiles[0].password);
    TEST_ASSERT_EQUAL_STRING("ws://192.168.1.99:8765", store.profiles[0].server_url);
}

TEST_CASE("upsert evicts the oldest profile when the store is full", "[wifi_profiles]")
{
    wifi_profile_store_t store = { 0 };
    wifi_profiles_upsert(&store, "net0", "p0");
    wifi_profiles_upsert(&store, "net1", "p1");
    wifi_profiles_upsert(&store, "net2", "p2");
    wifi_profiles_upsert(&store, "net3", "p3");
    wifi_profiles_upsert(&store, "net4", "p4");
    strncpy(store.profiles[4].server_url, "ws://stale:8765", WIFI_PROFILE_SERVER_URL_LEN);
    TEST_ASSERT_EQUAL(WIFI_PROFILES_MAX, store.count);

    wifi_profiles_upsert(&store, "net5", "p5");

    TEST_ASSERT_EQUAL(WIFI_PROFILES_MAX, store.count);
    // The new tail slot must not inherit "net4"'s old server_url from
    // before the eviction shift -- see wifi_profiles_upsert()'s comment on
    // why the new slot is memset first.
    TEST_ASSERT_EQUAL_STRING("", store.profiles[WIFI_PROFILES_MAX - 1].server_url);
    TEST_ASSERT_EQUAL_STRING("net1", store.profiles[0].ssid);
    TEST_ASSERT_EQUAL_STRING("net5", store.profiles[WIFI_PROFILES_MAX - 1].ssid);
}

TEST_CASE("find_best_match returns -1 for an empty store", "[wifi_profiles]")
{
    wifi_profile_store_t store = { 0 };
    wifi_scan_result_t results[] = { { .ssid = "WIFI_CASA", .rssi = -50 } };
    TEST_ASSERT_EQUAL(-1, wifi_profiles_find_best_match(&store, results, 1));
}

TEST_CASE("find_best_match returns -1 when no remembered network is visible", "[wifi_profiles]")
{
    wifi_profile_store_t store = { 0 };
    wifi_profiles_upsert(&store, "WIFI_CASA", "casa-pass");
    wifi_scan_result_t results[] = { { .ssid = "Vodafone-Vicino", .rssi = -60 } };
    TEST_ASSERT_EQUAL(-1, wifi_profiles_find_best_match(&store, results, 1));
}

TEST_CASE("find_best_match finds the single remembered network in range", "[wifi_profiles]")
{
    wifi_profile_store_t store = { 0 };
    wifi_profiles_upsert(&store, "WIFI_CASA", "casa-pass");
    wifi_profiles_upsert(&store, "2RStudio_interna", "ufficio-pass");
    wifi_scan_result_t results[] = {
        { .ssid = "Vodafone-Vicino", .rssi = -40 },
        { .ssid = "WIFI_CASA", .rssi = -55 },
    };
    TEST_ASSERT_EQUAL(0, wifi_profiles_find_best_match(&store, results, 2));
}

TEST_CASE("find_best_match picks the strongest signal when multiple remembered networks are in range", "[wifi_profiles]")
{
    wifi_profile_store_t store = { 0 };
    wifi_profiles_upsert(&store, "WIFI_CASA", "casa-pass");
    wifi_profiles_upsert(&store, "2RStudio_interna", "ufficio-pass");
    wifi_scan_result_t results[] = {
        { .ssid = "WIFI_CASA", .rssi = -70 },
        { .ssid = "2RStudio_interna", .rssi = -45 },
    };
    TEST_ASSERT_EQUAL(1, wifi_profiles_find_best_match(&store, results, 2));
}
