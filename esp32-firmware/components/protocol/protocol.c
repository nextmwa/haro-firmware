#include "protocol.h"
#include "cJSON.h"
#include <string.h>

char *protocol_encode_hello(const char *session_id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "session_id", session_id);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *protocol_encode_end_of_speech(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "end_of_speech");
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

esp_err_t protocol_parse_server_message(const char *text, protocol_event_t *out)
{
    cJSON *root = cJSON_Parse(text);
    if (root == NULL || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    esp_err_t result = ESP_OK;

    if (strcmp(type->valuestring, "emotion") == 0) {
        cJSON *value = cJSON_GetObjectItem(root, "value");
        if (!cJSON_IsString(value)) {
            result = ESP_ERR_INVALID_ARG;
        } else {
            out->type = PROTOCOL_EVENT_EMOTION;
            strncpy(out->value, value->valuestring, sizeof(out->value) - 1);
        }
    } else if (strcmp(type->valuestring, "response_end") == 0) {
        out->type = PROTOCOL_EVENT_RESPONSE_END;
    } else if (strcmp(type->valuestring, "error") == 0) {
        cJSON *message = cJSON_GetObjectItem(root, "message");
        out->type = PROTOCOL_EVENT_ERROR;
        if (cJSON_IsString(message)) {
            strncpy(out->message, message->valuestring, sizeof(out->message) - 1);
        }
    } else {
        result = ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(root);
    return result;
}
