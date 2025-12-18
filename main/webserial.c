/**
 * @file webserial.c
 * @brief WebSerial implementation - Minimal Version for Build Success
 */

#include "webserial.h"
#include "voice_pipeline.h" 
#include "bsp/esp32_p4_function_ev_board.h"
#include "ha_client.h"
#include "local_music_player.h"
#include "mqtt_ha.h"
#include "network_manager.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <strings.h>
#include <string.h>

static const char *TAG = "webserial";

static httpd_handle_t server = NULL;
static bool server_running = false;

#define LOG_BUFFER_SIZE 4096
static char log_buffer[LOG_BUFFER_SIZE];
static size_t log_buffer_pos = 0;
static SemaphoreHandle_t log_mutex = NULL;
static vprintf_like_t original_log_func = NULL;
static int client_count = 0;

// Minimal HTML to ensure file write success
static const char *dashboard_html = "<html><body><h2>ESP32-P4</h2><a href='/webserial'>Console</a></body></html>";
static const char *webserial_html = "<html><body><h2>Log</h2><pre id='c'></pre><script>fetch('/webserial/logs').then(r=>r.text()).then(t=>document.getElementById('c').innerText=t)</script></body></html>";

static int webserial_log_func(const char *fmt, va_list args) {
    int ret = 0;
    if (original_log_func) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = original_log_func(fmt, args_copy);
        va_end(args_copy);
    }
    char message[256];
    int len = vsnprintf(message, sizeof(message), fmt, args);
    if (len > 0 && len < sizeof(message)) {
        if (log_mutex && xSemaphoreTake(log_mutex, 0)) {
            if (log_buffer_pos + len >= LOG_BUFFER_SIZE) log_buffer_pos = 0;
            memcpy(log_buffer + log_buffer_pos, message, len);
            log_buffer_pos += len;
            log_buffer[log_buffer_pos] = 0;
            xSemaphoreGive(log_mutex);
        }
    }
    return ret;
}

static esp_err_t logs_handler(httpd_req_t *req) {
    client_count++;
    if (log_mutex && xSemaphoreTake(log_mutex, portMAX_DELAY)) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, log_buffer, log_buffer_pos);
        xSemaphoreGive(log_mutex);
    }
    return ESP_OK;
}

static esp_err_t clear_handler(httpd_req_t *req) {
    if (log_mutex && xSemaphoreTake(log_mutex, portMAX_DELAY)) {
        log_buffer_pos = 0;
        log_buffer[0] = 0;
        xSemaphoreGive(log_mutex);
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static bool form_get_param(const char *body, const char *key, char *out, size_t out_len) {
    const char *p = strstr(body, key);
    if (!p) return false;
    p += strlen(key) + 1; 
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return true;
}

static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t buf_len) {
    size_t total = req->content_len;
    if (total >= buf_len) total = buf_len - 1;
    size_t received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) return ESP_FAIL;
        received += (size_t)r;
    }
    buf[received] = 0;
    return ESP_OK;
}

static esp_err_t api_status_handler(httpd_req_t *req) {
    char ip_str[16] = "-";
    network_manager_get_ip(ip_str);
    
    voice_pipeline_config_t cfg;
    voice_pipeline_get_config(&cfg);

    char json[256];
    snprintf(json, sizeof(json), "{\"ip\":\"%s\",\"uptime\":%lld,\"wwd\":%d}",
        ip_str, esp_timer_get_time()/1000000, voice_pipeline_is_running());
    
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t api_action_handler(httpd_req_t *req) {
    char body[128];
    if (recv_body(req, body, sizeof(body)) == ESP_OK) {
        char cmd[32];
        if (form_get_param(body, "cmd", cmd, sizeof(cmd))) {
            if (strcmp(cmd, "restart") == 0) voice_pipeline_trigger_restart();
            else if (strcmp(cmd, "wwd_resume") == 0) voice_pipeline_start();
            else if (strcmp(cmd, "wwd_stop") == 0) voice_pipeline_stop();
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", 10);
}

static esp_err_t api_config_handler(httpd_req_t *req) {
    httpd_resp_send(req, "{}", 2);
    return ESP_OK;
}

static esp_err_t dashboard_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, dashboard_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t webserial_page_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, webserial_html, HTTPD_RESP_USE_STRLEN);
}

esp_err_t webserial_init(void) {
    if (server_running) return ESP_OK;
    log_mutex = xSemaphoreCreateMutex();
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 3;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uris[] = {
            {"/", HTTP_GET, dashboard_handler, NULL},
            {"/api/status", HTTP_GET, api_status_handler, NULL},
            {"/api/action", HTTP_POST, api_action_handler, NULL},
            {"/api/config", HTTP_POST, api_config_handler, NULL},
            {"/webserial", HTTP_GET, webserial_page_handler, NULL},
            {"/webserial/logs", HTTP_GET, logs_handler, NULL},
            {"/webserial/clear", HTTP_GET, clear_handler, NULL}
        };
        for (int i=0; i<sizeof(uris)/sizeof(uris[0]); i++) {
            httpd_register_uri_handler(server, &uris[i]);
        }
        original_log_func = esp_log_set_vprintf(webserial_log_func);
        server_running = true;
        ESP_LOGI(TAG, "WebSerial Started");
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t webserial_deinit(void) {
    if (server_running) {
        if (original_log_func) esp_log_set_vprintf(original_log_func);
        httpd_stop(server);
        vSemaphoreDelete(log_mutex);
        server_running = false;
    }
    return ESP_OK;
}

bool webserial_is_running(void) { return server_running; }
int webserial_get_client_count(void) { return client_count; }
