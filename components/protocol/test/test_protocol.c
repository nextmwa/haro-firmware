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

TEST_CASE("encode_interrupt produces the expected JSON", "[protocol]")
{
    char *json = protocol_encode_interrupt();
    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_EQUAL_STRING("interrupt", cJSON_GetObjectItem(root, "type")->valuestring);
    cJSON_Delete(root);
    free(json);
}

TEST_CASE("encode_camera_frame base64-encodes the JPEG bytes", "[protocol]")
{
    const uint8_t jpeg[] = { 0xFF, 0xD8, 0xFF, 0xE0, 'f', 'a', 'k', 'e' };
    char *json = protocol_encode_camera_frame(jpeg, sizeof(jpeg));
    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_STRING("camera_frame", cJSON_GetObjectItem(root, "type")->valuestring);
    // Standard base64 of the bytes above (verified against `base64` CLI).
    TEST_ASSERT_EQUAL_STRING("/9j/4GZha2U=", cJSON_GetObjectItem(root, "data")->valuestring);
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

TEST_CASE("parse_server_message decodes an action event with an integer result", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message(
        "{\"type\": \"action\", \"name\": \"dice_roll\", \"result\": 4}", &evt);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(PROTOCOL_EVENT_ACTION, evt.type);
    TEST_ASSERT_EQUAL_STRING("dice_roll", evt.action_name);
    TEST_ASSERT_EQUAL_STRING("4", evt.action_result);
}

TEST_CASE("parse_server_message decodes an action event with a string result", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message(
        "{\"type\": \"action\", \"name\": \"coin_flip\", \"result\": \"testa\"}", &evt);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(PROTOCOL_EVENT_ACTION, evt.type);
    TEST_ASSERT_EQUAL_STRING("coin_flip", evt.action_name);
    TEST_ASSERT_EQUAL_STRING("testa", evt.action_result);
}

TEST_CASE("parse_server_message decodes a music_playing action with a track label", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message(
        "{\"type\": \"action\", \"name\": \"music_playing\", \"result\": \"Vita Spericolata - Vasco Rossi\"}", &evt);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(PROTOCOL_EVENT_ACTION, evt.type);
    TEST_ASSERT_EQUAL_STRING("music_playing", evt.action_name);
    TEST_ASSERT_EQUAL_STRING("Vita Spericolata - Vasco Rossi", evt.action_result);
}

TEST_CASE("parse_server_message decodes a found face_position event", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message(
        "{\"type\": \"face_position\", \"found\": true, \"dx\": 0.21, \"dy\": -0.06}", &evt);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(PROTOCOL_EVENT_FACE_POSITION, evt.type);
    TEST_ASSERT_TRUE(evt.face_found);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.21f, evt.face_dx);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -0.06f, evt.face_dy);
}

TEST_CASE("parse_server_message decodes a not-found face_position event", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message("{\"type\": \"face_position\", \"found\": false}", &evt);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL(PROTOCOL_EVENT_FACE_POSITION, evt.type);
    TEST_ASSERT_FALSE(evt.face_found);
}

TEST_CASE("parse_server_message rejects a face_position event missing found", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message("{\"type\": \"face_position\"}", &evt);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

TEST_CASE("parse_server_message rejects a found face_position event missing dx/dy", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message("{\"type\": \"face_position\", \"found\": true}", &evt);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

TEST_CASE("parse_server_message rejects an action event missing result", "[protocol]")
{
    protocol_event_t evt;
    esp_err_t err = protocol_parse_server_message(
        "{\"type\": \"action\", \"name\": \"dice_roll\"}", &evt);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
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
