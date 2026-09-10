#include "unity.h"
#include "unity_test_runner.h"
#include "protocol.h"
#include "cJSON.h"
#include <string.h>

TEST_CASE("encode_hello produces the expected JSON", "[protocol]")
{
    char *json = protocol_encode_hello("haro-session");
    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_STRING("hello", cJSON_GetObjectItem(root, "type")->valuestring);
    TEST_ASSERT_EQUAL_STRING("haro-session", cJSON_GetObjectItem(root, "session_id")->valuestring);
    cJSON_Delete(root);
    free(json);
}

TEST_CASE("encode_end_of_speech produces the expected JSON", "[protocol]")
{
    char *json = protocol_encode_end_of_speech();
    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_EQUAL_STRING("end_of_speech", cJSON_GetObjectItem(root, "type")->valuestring);
    cJSON_Delete(root);
    free(json);
}

TEST_CASE("parse_server_message decodes an emotion event", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message(
        "{\"type\": \"emotion\", \"value\": \"happy\"}", &evt);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(PROTOCOL_EVENT_EMOTION, evt.type);
    TEST_ASSERT_EQUAL_STRING("happy", evt.value);
}

TEST_CASE("parse_server_message decodes a response_end event", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message(
        "{\"type\": \"response_end\"}", &evt);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(PROTOCOL_EVENT_RESPONSE_END, evt.type);
}

TEST_CASE("parse_server_message decodes an error event", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message(
        "{\"type\": \"error\", \"message\": \"boom\"}", &evt);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(PROTOCOL_EVENT_ERROR, evt.type);
    TEST_ASSERT_EQUAL_STRING("boom", evt.message);
}

TEST_CASE("parse_server_message rejects invalid JSON", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message("not json", &evt);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

TEST_CASE("parse_server_message rejects an unknown type", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message("{\"type\": \"mystery\"}", &evt);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}
