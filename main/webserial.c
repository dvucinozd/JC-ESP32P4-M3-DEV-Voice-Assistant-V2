/**
 * @file webserial.c
 * @brief WebSerial implementation - Remote serial console
 */

#include "webserial.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "webserial";

// HTTP server handle
static httpd_handle_t server = NULL;
static bool server_running = false;

// WebSocket client tracking
#define MAX_CLIENTS 4
static int ws_clients[MAX_CLIENTS];
static int client_count = 0;
static SemaphoreHandle_t clients_mutex = NULL;

// Log buffer for new clients
#define LOG_BUFFER_SIZE 4096
static char log_buffer[LOG_BUFFER_SIZE];
static size_t log_buffer_pos = 0;
static SemaphoreHandle_t log_mutex = NULL;

// Original log function pointer
static vprintf_like_t original_log_func = NULL;

/**
 * @brief HTML page for WebSerial interface
 */
static const char *webserial_html =
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP32-P4 WebSerial</title>"
"<style>"
"body{font-family:monospace;margin:0;padding:10px;background:#1e1e1e;color:#d4d4d4}"
"#console{background:#000;color:#0f0;padding:10px;height:80vh;overflow-y:auto;border:1px solid #333;white-space:pre-wrap;word-wrap:break-word}"
"#input{width:calc(100% - 100px);padding:5px;font-family:monospace;background:#333;color:#fff;border:1px solid #666}"
"#send{padding:5px 15px;background:#007acc;color:#fff;border:none;cursor:pointer}"
"#send:hover{background:#005a9e}"
".status{padding:5px;margin-bottom:5px;background:#2d2d2d;border-left:3px solid #007acc}"
".connected{border-left-color:#0f0}"
".disconnected{border-left-color:#f00}"
"</style>"
"</head>"
"<body>"
"<h2>ESP32-P4 Voice Assistant - WebSerial Console</h2>"
"<div id='status' class='status disconnected'>Disconnected</div>"
"<div id='console'></div>"
"<div style='margin-top:10px'>"
"<input type='text' id='input' placeholder='Type command and press Enter...' />"
"<button id='send'>Send</button>"
"<button id='clear' onclick='clearConsole()'>Clear</button>"
"</div>"
"<script>"
"let ws;let console_div=document.getElementById('console');"
"let status_div=document.getElementById('status');"
"let input=document.getElementById('input');"
"function connect(){"
"ws=new WebSocket('ws://'+location.host+'/ws');"
"ws.onopen=function(){"
"status_div.textContent='Connected';status_div.className='status connected';"
"};"
"ws.onmessage=function(e){"
"console_div.textContent+=e.data;console_div.scrollTop=console_div.scrollHeight;"
"};"
"ws.onerror=function(){status_div.textContent='Error';status_div.className='status disconnected';};"
"ws.onclose=function(){"
"status_div.textContent='Disconnected - Reconnecting...';status_div.className='status disconnected';"
"setTimeout(connect,2000);"
"};"
"}"
"function send(){"
"if(ws&&ws.readyState===WebSocket.OPEN&&input.value){"
"ws.send(input.value+'\\n');input.value='';"
"}"
"}"
"function clearConsole(){console_div.textContent='';}"
"input.addEventListener('keypress',function(e){if(e.key==='Enter')send();});"
"document.getElementById('send').onclick=send;"
"connect();"
"</script>"
"</body>"
"</html>";

/**
 * @brief Add client to tracking list
 */
static bool add_client(int fd)
{
    xSemaphoreTake(clients_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i] == -1) {
            ws_clients[i] = fd;
            client_count++;
            ESP_LOGI(TAG, "Client %d connected (total: %d)", fd, client_count);
            xSemaphoreGive(clients_mutex);
            return true;
        }
    }

    xSemaphoreGive(clients_mutex);
    ESP_LOGW(TAG, "Max clients reached, rejecting fd %d", fd);
    return false;
}

/**
 * @brief Remove client from tracking list
 */
static void remove_client(int fd)
{
    xSemaphoreTake(clients_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i] == fd) {
            ws_clients[i] = -1;
            client_count--;
            ESP_LOGI(TAG, "Client %d disconnected (total: %d)", fd, client_count);
            break;
        }
    }

    xSemaphoreGive(clients_mutex);
}

/**
 * @brief Custom log function that sends to both UART and WebSocket
 */
static int webserial_log_func(const char *fmt, va_list args)
{
    // Call original log function (UART output)
    int ret = 0;
    if (original_log_func) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = original_log_func(fmt, args_copy);
        va_end(args_copy);
    }

    // Format message for WebSocket
    char message[256];
    int len = vsnprintf(message, sizeof(message), fmt, args);

    if (len > 0 && len < sizeof(message)) {
        // Add to circular log buffer
        xSemaphoreTake(log_mutex, portMAX_DELAY);

        if (log_buffer_pos + len >= LOG_BUFFER_SIZE) {
            // Buffer full, shift contents
            memmove(log_buffer, log_buffer + LOG_BUFFER_SIZE / 2, LOG_BUFFER_SIZE / 2);
            log_buffer_pos = LOG_BUFFER_SIZE / 2;
        }

        memcpy(log_buffer + log_buffer_pos, message, len);
        log_buffer_pos += len;
        log_buffer[log_buffer_pos] = '\0';

        xSemaphoreGive(log_mutex);

        // Broadcast to WebSocket clients
        webserial_broadcast(message, len);
    }

    return ret;
}

/**
 * @brief WebSocket handler
 */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake for fd %d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    // Receive frame
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ws_pkt.len) {
        uint8_t *buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate buffer");
            return ESP_ERR_NO_MEM;
        }

        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed: %s", esp_err_to_name(ret));
            free(buf);
            return ret;
        }

        // Echo received command to console
        ESP_LOGI(TAG, "WebSerial command: %s", (char *)ws_pkt.payload);

        free(buf);
    }

    return ESP_OK;
}

/**
 * @brief Root page handler - serves HTML interface
 */
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, webserial_html, strlen(webserial_html));
}

/**
 * @brief Broadcast message to all connected WebSocket clients
 */
esp_err_t webserial_broadcast(const char *message, size_t length)
{
    if (!server_running || client_count == 0) {
        return ESP_OK; // No clients connected
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t *)message;
    ws_pkt.len = length;

    xSemaphoreTake(clients_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (ws_clients[i] != -1) {
            esp_err_t ret = httpd_ws_send_frame_async(server, ws_clients[i], &ws_pkt);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send to client %d: %s", ws_clients[i], esp_err_to_name(ret));
            }
        }
    }

    xSemaphoreGive(clients_mutex);
    return ESP_OK;
}

/**
 * @brief WebSocket open callback
 */
static esp_err_t ws_open_handler(httpd_handle_t hd, int sockfd)
{
    ESP_LOGI(TAG, "New WebSocket connection: fd=%d", sockfd);

    if (!add_client(sockfd)) {
        return ESP_FAIL;
    }

    // Send log buffer to new client
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    if (log_buffer_pos > 0) {
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;
        ws_pkt.payload = (uint8_t *)log_buffer;
        ws_pkt.len = log_buffer_pos;

        httpd_ws_send_frame_async(hd, sockfd, &ws_pkt);
    }
    xSemaphoreGive(log_mutex);

    return ESP_OK;
}

/**
 * @brief WebSocket close callback
 */
static void ws_close_handler(httpd_handle_t hd, int sockfd)
{
    ESP_LOGI(TAG, "WebSocket connection closed: fd=%d", sockfd);
    remove_client(sockfd);
}

/**
 * @brief Initialize WebSerial server
 */
esp_err_t webserial_init(void)
{
    if (server_running) {
        ESP_LOGW(TAG, "WebSerial already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WebSerial server...");

    // Create mutexes
    clients_mutex = xSemaphoreCreateMutex();
    log_mutex = xSemaphoreCreateMutex();
    if (!clients_mutex || !log_mutex) {
        ESP_LOGE(TAG, "Failed to create mutexes");
        return ESP_FAIL;
    }

    // Initialize client list
    for (int i = 0; i < MAX_CLIENTS; i++) {
        ws_clients[i] = -1;
    }
    client_count = 0;

    // Configure HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    config.open_fn = ws_open_handler;
    config.close_fn = ws_close_handler;

    // Start HTTP server
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register URI handlers
    httpd_uri_t root_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = root_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &root_uri);

    httpd_uri_t ws_uri = {
        .uri       = "/ws",
        .method    = HTTP_GET,
        .handler   = ws_handler,
        .user_ctx  = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &ws_uri);

    // Hook into ESP log system
    original_log_func = esp_log_set_vprintf(webserial_log_func);

    server_running = true;
    ESP_LOGI(TAG, "WebSerial server started successfully");
    ESP_LOGI(TAG, "Access WebSerial at: http://<device-ip>/");

    return ESP_OK;
}

/**
 * @brief Deinitialize WebSerial server
 */
esp_err_t webserial_deinit(void)
{
    if (!server_running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping WebSerial server...");

    // Restore original log function
    if (original_log_func) {
        esp_log_set_vprintf(original_log_func);
        original_log_func = NULL;
    }

    // Stop HTTP server
    if (server) {
        httpd_stop(server);
        server = NULL;
    }

    // Delete mutexes
    if (clients_mutex) {
        vSemaphoreDelete(clients_mutex);
        clients_mutex = NULL;
    }
    if (log_mutex) {
        vSemaphoreDelete(log_mutex);
        log_mutex = NULL;
    }

    server_running = false;
    client_count = 0;

    ESP_LOGI(TAG, "WebSerial server stopped");
    return ESP_OK;
}

/**
 * @brief Check if WebSerial is running
 */
bool webserial_is_running(void)
{
    return server_running;
}

/**
 * @brief Get number of connected WebSerial clients
 */
int webserial_get_client_count(void)
{
    return client_count;
}
