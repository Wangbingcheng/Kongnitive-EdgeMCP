/*
 * JSON-RPC 2.0 Message Handler Implementation
 */

#include "jsonrpc.h"
#include <string.h>
#include <esp_log.h>

static const char *TAG = "jsonrpc";

esp_err_t jsonrpc_parse_message(const char *json_str, jsonrpc_message_t *msg)
{
    if (!json_str || !msg) {
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize message structure
    memset(msg, 0, sizeof(jsonrpc_message_t));
    msg->params = NULL;
    msg->result = NULL;

    // Parse JSON
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return ESP_ERR_INVALID_ARG;
    }

    // Validate JSON-RPC version
    cJSON *jsonrpc = cJSON_GetObjectItem(root, "jsonrpc");
    if (!jsonrpc || !cJSON_IsString(jsonrpc) || strcmp(jsonrpc->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid or missing jsonrpc version");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    // Check for ID field
    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (id) {
        msg->has_id = true;
        if (cJSON_IsNumber(id)) {
            msg->id = id->valueint;
        } else if (cJSON_IsString(id)) {
            // Try to parse string ID as number
            msg->id = atoi(id->valuestring);
        } else {
            msg->id = 0;
        }
    } else {
        msg->has_id = false;
        msg->id = 0;
    }

    // Check if this is a request or response
    cJSON *method = cJSON_GetObjectItem(root, "method");
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *error = cJSON_GetObjectItem(root, "error");

    if (method && cJSON_IsString(method)) {
        // This is a request
        msg->type = msg->has_id ? JSONRPC_REQUEST : JSONRPC_NOTIFICATION;
        strncpy(msg->method, method->valuestring, sizeof(msg->method) - 1);
        msg->method[sizeof(msg->method) - 1] = '\0';

        // Transfer ownership instead of duplicating to avoid memory pressure
        cJSON *params = cJSON_DetachItemFromObject(root, "params");
        msg->params = params;
    } else if (result) {
        // This is a success response
        msg->type = JSONRPC_RESPONSE;
        msg->result = cJSON_DetachItemFromObject(root, "result");
    } else if (error) {
        // This is an error response
        msg->type = JSONRPC_ERROR;
        cJSON *code = cJSON_GetObjectItem(error, "code");
        cJSON *message = cJSON_GetObjectItem(error, "message");
        
        if (code && cJSON_IsNumber(code)) {
            msg->error_code = code->valueint;
        }
        if (message && cJSON_IsString(message)) {
            strncpy(msg->error_message, message->valuestring, sizeof(msg->error_message) - 1);
            msg->error_message[sizeof(msg->error_message) - 1] = '\0';
        }
    } else {
        ESP_LOGE(TAG, "Invalid JSON-RPC message: no method, result, or error");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

char* jsonrpc_create_response(int id, cJSON *result)
{
    ESP_LOGD(TAG, ">>> jsonrpc_create_response called: id=%d, result=%p", id, result);
    
    if (!result) {
        ESP_LOGE(TAG, "jsonrpc_create_response called with NULL result!");
        return NULL;
    }

    // Print result first for debug
    char *result_str = cJSON_Print(result);
    ESP_LOGD(TAG, ">>> result JSON: %s", result_str ? result_str : "(null)");
    if (result_str) free(result_str);

    // Create response and manually build it
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        ESP_LOGE(TAG, "Failed to create response object");
        return NULL;
    }

    cJSON_AddItemToObject(response, "jsonrpc", cJSON_CreateString("2.0"));
    cJSON_AddItemToObject(response, "id", cJSON_CreateNumber(id));
    
    // For result, we need to handle it carefully
    // If it's an object/array, we can add it directly; otherwise wrap it
    if (result->type & cJSON_Object) {
        // result is already an object, move its children to response
        cJSON_AddItemToObject(response, "result", result);
    } else if (result->type & cJSON_Array) {
        cJSON_AddItemToObject(response, "result", result);
    } else {
        // Primitive type, wrap in a generic object
        cJSON *result_obj = cJSON_CreateObject();
        cJSON_AddItemToObject(result_obj, "value", result);
        cJSON_AddItemToObject(response, "result", result_obj);
    }

    char *json_str = cJSON_PrintUnformatted(response);
    
    if (json_str) {
        ESP_LOGD(TAG, ">>> Response JSON: %s", json_str);
    } else {
        ESP_LOGE(TAG, ">>> cJSON_PrintUnformatted FAILED!");
    }
    
    cJSON_Delete(response);
    return json_str;
}

char* jsonrpc_create_error(int id, int code, const char *message)
{
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        ESP_LOGE(TAG, "Failed to create error response object");
        return NULL;
    }

    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    if (id != 0) {
        cJSON_AddNumberToObject(response, "id", id);
    } else {
        cJSON_AddNullToObject(response, "id");
    }

    cJSON *error = cJSON_CreateObject();
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message ? message : "Unknown error");
    cJSON_AddItemToObject(response, "error", error);

    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);

    return json_str;
}

void jsonrpc_message_cleanup(jsonrpc_message_t *msg)
{
    if (!msg) {
        return;
    }

    if (msg->params) {
        cJSON_Delete(msg->params);
        msg->params = NULL;
    }

    if (msg->result) {
        cJSON_Delete(msg->result);
        msg->result = NULL;
    }
}
