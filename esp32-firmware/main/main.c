#include "esp_log.h"
#include "unity.h"
#include "unity_test_runner.h"
#include "protocol.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "haro";

int test_count = 0;
int test_passed = 0;

#define TEST_LOG(fmt, ...) printf("[TEST] " fmt "\n", ##__VA_ARGS__)

void run_manual_tests(void)
{
    protocol_event_t evt;
    char *json;
    cJSON *root;

    // Test 1: encode_hello
    TEST_LOG("Test 1: encode_hello produces the expected JSON");
    test_count++;
    json = protocol_encode_hello("haro-session");
    root = cJSON_Parse(json);
    if (root && cJSON_GetObjectItem(root, "type") &&
        strcmp(cJSON_GetObjectItem(root, "type")->valuestring, "hello") == 0 &&
        cJSON_GetObjectItem(root, "session_id") &&
        strcmp(cJSON_GetObjectItem(root, "session_id")->valuestring, "haro-session") == 0) {
        TEST_LOG("  PASS");
        test_passed++;
    } else {
        TEST_LOG("  FAIL");
    }
    if (root) cJSON_Delete(root);
    free(json);

    // Test 2: encode_end_of_speech
    TEST_LOG("Test 2: encode_end_of_speech produces the expected JSON");
    test_count++;
    json = protocol_encode_end_of_speech();
    root = cJSON_Parse(json);
    if (root && cJSON_GetObjectItem(root, "type") &&
        strcmp(cJSON_GetObjectItem(root, "type")->valuestring, "end_of_speech") == 0) {
        TEST_LOG("  PASS");
        test_passed++;
    } else {
        TEST_LOG("  FAIL");
    }
    if (root) cJSON_Delete(root);
    free(json);

    // Test 3: parse emotion event
    TEST_LOG("Test 3: parse_server_message decodes an emotion event");
    test_count++;
    if (protocol_parse_server_message("{\"type\": \"emotion\", \"value\": \"happy\"}", &evt) == ESP_OK &&
        evt.type == PROTOCOL_EVENT_EMOTION &&
        strcmp(evt.value, "happy") == 0) {
        TEST_LOG("  PASS");
        test_passed++;
    } else {
        TEST_LOG("  FAIL");
    }

    // Test 4: parse response_end event
    TEST_LOG("Test 4: parse_server_message decodes a response_end event");
    test_count++;
    if (protocol_parse_server_message("{\"type\": \"response_end\"}", &evt) == ESP_OK &&
        evt.type == PROTOCOL_EVENT_RESPONSE_END) {
        TEST_LOG("  PASS");
        test_passed++;
    } else {
        TEST_LOG("  FAIL");
    }

    // Test 5: parse error event
    TEST_LOG("Test 5: parse_server_message decodes an error event");
    test_count++;
    if (protocol_parse_server_message("{\"type\": \"error\", \"message\": \"boom\"}", &evt) == ESP_OK &&
        evt.type == PROTOCOL_EVENT_ERROR &&
        strcmp(evt.message, "boom") == 0) {
        TEST_LOG("  PASS");
        test_passed++;
    } else {
        TEST_LOG("  FAIL");
    }

    // Test 6: reject invalid JSON
    TEST_LOG("Test 6: parse_server_message rejects invalid JSON");
    test_count++;
    if (protocol_parse_server_message("not json", &evt) == ESP_ERR_INVALID_ARG) {
        TEST_LOG("  PASS");
        test_passed++;
    } else {
        TEST_LOG("  FAIL");
    }

    // Test 7: reject unknown type
    TEST_LOG("Test 7: parse_server_message rejects an unknown type");
    test_count++;
    if (protocol_parse_server_message("{\"type\": \"mystery\"}", &evt) == ESP_ERR_INVALID_ARG) {
        TEST_LOG("  PASS");
        test_passed++;
    } else {
        TEST_LOG("  FAIL");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Haro firmware starting");
    printf("\n\n========== Running Protocol Tests ==========\n\n");
    fflush(stdout);

    run_manual_tests();

    printf("\n========== Test Summary: %d/%d PASSED ==========\n\n", test_passed, test_count);
    fflush(stdout);
}
