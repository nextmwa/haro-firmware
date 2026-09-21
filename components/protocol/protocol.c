#include "protocol.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include <stdio.h>
#include <stdlib.h>
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

char *protocol_encode_camera_frame(const uint8_t *jpeg, size_t jpeg_len)
{
    // mbedtls_base64_encode's documented "call with dlen=0 to get the
    // required size in *olen" pattern -- avoids guessing/over-allocating.
    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, jpeg, jpeg_len);
    unsigned char *b64_buf = malloc(b64_len + 1); // +1: mbedtls_base64_encode does NOT null-terminate
    if (b64_buf == NULL) {
        return NULL;
    }

    size_t written = 0;
    if (mbedtls_base64_encode(b64_buf, b64_len, &written, jpeg, jpeg_len) != 0) {
        free(b64_buf);
        return NULL;
    }
    b64_buf[written] = '\0';

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "camera_frame");
    cJSON_AddStringToObject(root, "data", (const char *)b64_buf);
    free(b64_buf);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *protocol_encode_interrupt(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "interrupt");
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
    } else if (strcmp(type->valuestring, "action") == 0) {
        cJSON *name = cJSON_GetObjectItem(root, "name");
        cJSON *action_result = cJSON_GetObjectItem(root, "result");
        if (!cJSON_IsString(name) || action_result == NULL) {
            result = ESP_ERR_INVALID_ARG;
        } else {
            out->type = PROTOCOL_EVENT_ACTION;
            strncpy(out->action_name, name->valuestring, sizeof(out->action_name) - 1);
            // "result" is a JSON number (dice) or string (coin) depending
            // on the action -- format either into the one string field
            // protocol_event_t carries (see its struct comment).
            if (cJSON_IsString(action_result)) {
                strncpy(out->action_result, action_result->valuestring, sizeof(out->action_result) - 1);
            } else if (cJSON_IsNumber(action_result)) {
                snprintf(out->action_result, sizeof(out->action_result), "%d", action_result->valueint);
            } else {
                result = ESP_ERR_INVALID_ARG;
            }
        }
    } else if (strcmp(type->valuestring, "face_position") == 0) {
        cJSON *found = cJSON_GetObjectItem(root, "found");
        if (!cJSON_IsBool(found)) {
            result = ESP_ERR_INVALID_ARG;
        } else {
            out->type = PROTOCOL_EVENT_FACE_POSITION;
            out->face_found = cJSON_IsTrue(found);
            if (out->face_found) {
                cJSON *dx = cJSON_GetObjectItem(root, "dx");
                cJSON *dy = cJSON_GetObjectItem(root, "dy");
                if (!cJSON_IsNumber(dx) || !cJSON_IsNumber(dy)) {
                    result = ESP_ERR_INVALID_ARG;
                } else {
                    out->face_dx = (float)dx->valuedouble;
                    out->face_dy = (float)dy->valuedouble;
                }
            }
        }
    } else {
        result = ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(root);
    return result;
}
