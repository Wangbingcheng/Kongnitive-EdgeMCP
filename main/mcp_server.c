/*
 * MCP Server Core Implementation
 */

#include "mcp_server.h"
#include "jsonrpc.h"
#include "mcp_protocol.h"
#include <string.h>
#include <stdlib.h>
#include <esp_log.h>

static const char *TAG = "mcp_server";

#define WS_FRAME_BUF_SIZE CONFIG_MCP_MAX_MESSAGE_SIZE
static uint8_t s_ws_frame_buf[WS_FRAME_BUF_SIZE];
static char s_http_body_buf[CONFIG_MCP_MAX_MESSAGE_SIZE];

void json_pool_init_hooks(void)
{
    cJSON_Hooks hooks = {
        .malloc_fn = malloc,
        .free_fn = free,
    };
    cJSON_InitHooks(&hooks);
    ESP_LOGI(TAG, "cJSON hooks initialized");
}

// Method dispatch table
typedef struct {
    const char *method;
    esp_err_t (*handler)(cJSON *params, cJSON **result);
} mcp_method_entry_t;

static const mcp_method_entry_t method_table[] = {
    {"initialize", mcp_handle_initialize},
    {"tools/list", mcp_handle_tools_list},
    {"tools/call", mcp_handle_tools_call},
    {"ping", mcp_handle_ping},
    {NULL, NULL}  // Sentinel
};

esp_err_t mcp_server_init(void)
{
    ESP_LOGI(TAG, "Initializing MCP server");
    
    json_pool_init_hooks();
    
    esp_err_t ret = mcp_protocol_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MCP protocol: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "MCP server initialized successfully");
    return ESP_OK;
}

static esp_err_t mcp_dispatch_method(const char *method, cJSON *params, cJSON **result)
{
    if (!method || !result) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGD(TAG, "Dispatching method: '%s'", method);
    
    // Find method handler
    for (const mcp_method_entry_t *entry = method_table; entry->method != NULL; entry++) {
        ESP_LOGD(TAG, "Checking method table: '%s' vs '%s'", method, entry->method);
        if (strcmp(entry->method, method) == 0) {
            ESP_LOGD(TAG, "Found handler for: '%s'", method);
            return entry->handler(params, result);
        }
    }
    
    ESP_LOGW(TAG, "Method not found: %s", method);
    return ESP_ERR_NOT_FOUND;
}

char* mcp_server_process_message(const char *json_str)
{
    if (!json_str) {
        return jsonrpc_create_error(0, JSONRPC_INVALID_REQUEST, "Null message");
    }
    
    ESP_LOGD(TAG, "Processing message: %s", json_str);
    
    // Parse JSON-RPC message
    jsonrpc_message_t msg;
    esp_err_t err = jsonrpc_parse_message(json_str, &msg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse JSON-RPC message");
        return jsonrpc_create_error(0, JSONRPC_PARSE_ERROR, "Invalid JSON or JSON-RPC format");
    }
    
    char *response = NULL;
    
    // Handle request
    if (msg.type == JSONRPC_REQUEST) {
        ESP_LOGD(TAG, ">>> Processing request: method=%s, id=%d", msg.method, msg.id);
        cJSON *result = NULL;
        err = mcp_dispatch_method(msg.method, msg.params, &result);
        
        ESP_LOGD(TAG, ">>> Method result: err=%d, result=%p", err, result);
        
        if (err == ESP_OK && result) {
            response = jsonrpc_create_response(msg.id, result);
            ESP_LOGD(TAG, ">>> jsonrpc_create_response returned: %p", response);
            // Note: jsonrpc_create_response takes ownership of result, don't delete here
        } else if (err == ESP_ERR_NOT_FOUND) {
            response = jsonrpc_create_error(msg.id, JSONRPC_METHOD_NOT_FOUND, 
                                           "Method not found");
        } else if (err == ESP_ERR_INVALID_ARG) {
            response = jsonrpc_create_error(msg.id, JSONRPC_INVALID_PARAMS, 
                                           "Invalid parameters");
        } else {
            ESP_LOGW(TAG, "Method failed: err=%d, result=%p", err, result);
            response = jsonrpc_create_error(msg.id, JSONRPC_INTERNAL_ERROR, 
                                           "Internal error");
        }
    } else if (msg.type == JSONRPC_NOTIFICATION) {
        // Notifications don't get responses
        ESP_LOGD(TAG, "Received notification: %s", msg.method);
    } else {
        response = jsonrpc_create_error(0, JSONRPC_INVALID_REQUEST, 
                                       "Invalid message type");
    }
    
    // Cleanup
    jsonrpc_message_cleanup(&msg);
    
    return response;
}

esp_err_t mcp_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGD(TAG, "MCP client connected");
        return ESP_OK;
    }
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    
    // Get frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGD(TAG, "Received frame len: %d", ws_pkt.len);
    
    if (ws_pkt.len && ws_pkt.len <= WS_FRAME_BUF_SIZE) {
        // Use static buffer instead of malloc
        memset(s_ws_frame_buf, 0, WS_FRAME_BUF_SIZE);
        ws_pkt.payload = s_ws_frame_buf;
        
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
            return ret;
        }
        
        // Process message based on type
        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
            ESP_LOGD(TAG, "Received MCP message");
            
            // Process MCP message
            char *response = mcp_server_process_message((char*)ws_pkt.payload);
            
            if (response) {
                // Send response
                httpd_ws_frame_t resp_pkt;
                memset(&resp_pkt, 0, sizeof(httpd_ws_frame_t));
                resp_pkt.type = HTTPD_WS_TYPE_TEXT;
                resp_pkt.payload = (uint8_t*)response;
                resp_pkt.len = strlen(response);
                
                ret = httpd_ws_send_frame(req, &resp_pkt);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to send response: %s", esp_err_to_name(ret));
                }
                
                free(response);
            }
        } else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
            ESP_LOGD(TAG, "Received PING, sending PONG");
            ws_pkt.type = HTTPD_WS_TYPE_PONG;
            ret = httpd_ws_send_frame(req, &ws_pkt);
        } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
            ESP_LOGD(TAG, "Received CLOSE frame");
            ws_pkt.len = 0;
            ws_pkt.payload = NULL;
            ret = httpd_ws_send_frame(req, &ws_pkt);
        }
    } else if (ws_pkt.len > WS_FRAME_BUF_SIZE) {
        ESP_LOGE(TAG, "WebSocket frame too large: %d > %d", ws_pkt.len, WS_FRAME_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }
    
    return ret;
}

/* --- Streamable HTTP transport (POST /mcp) --- */

esp_err_t mcp_http_handler(httpd_req_t *req)
{
    /* Read POST body */
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > CONFIG_MCP_MAX_MESSAGE_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    char *body = s_http_body_buf;
    memset(body, 0, CONFIG_MCP_MAX_MESSAGE_SIZE);

    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, body + received, content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Timeout");
            }
            return ESP_FAIL;
        }
        received += ret;
    }
    body[content_len] = '\0';

    ESP_LOGD(TAG, "HTTP MCP request (%d bytes)", content_len);

    /* Process through the same MCP pipeline as WebSocket */
    char *response = mcp_server_process_message(body);

    if (response) {
        /* Normal request -> JSON response */
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, response, strlen(response));
        
        /* Always try to free - if it's static error, free won't hurt */
        free(response);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to process MCP message");
    }

    return ESP_OK;
}

/* --- GET /mcp server info --- */

esp_err_t mcp_info_handler(httpd_req_t *req)
{
    const char *info =
        "{\"name\":\"" MCP_SERVER_NAME "\","
        "\"version\":\"" MCP_SERVER_VERSION "\","
        "\"protocolVersion\":\"" MCP_PROTOCOL_VERSION "\","
        "\"transports\":[\"http-post\",\"websocket\"]}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, info, strlen(info));
    return ESP_OK;
}

/* --- PATCH /mcp (method not allowed) --- */

esp_err_t mcp_patch_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_hdr(req, "Allow", "GET, POST");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* --- OPTIONS /mcp (CORS preflight) --- */

esp_err_t mcp_options_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Allow", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}
