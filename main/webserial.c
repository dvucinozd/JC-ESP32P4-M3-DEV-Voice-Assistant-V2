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
#include "ota_update.h"
#include "led_status.h"
#include "esp_log.h"
#include "esp_err.h"
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

#define LOG_BUFFER_SIZE 8192
static char log_buffer[LOG_BUFFER_SIZE];
static size_t log_buffer_pos = 0;
static uint32_t log_seq = 0;
static uint32_t log_base_seq = 0;
static SemaphoreHandle_t log_mutex = NULL;
static vprintf_like_t original_log_func = NULL;
static int client_count = 0;

// Enhanced HTML for better control
static const char *dashboard_html = 
    "<html><head><title>ESP32-P4 Control</title>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>body{font-family:sans-serif;margin:20px;background:#f0f2f5} .card{background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);margin-bottom:20px} "
    "button{padding:10px 20px;margin:5px;cursor:pointer;border:none;border-radius:4px;background:#007bff;color:white} button:hover{background:#0056b3} "
    "input{padding:10px;width:100%;max-width:400px;margin-bottom:10px;border:1px solid #ddd;border-radius:4px}</style></head>"
    "<body>"
    "<h2>ESP32-P4 Voice Assistant</h2>"
    "<div class='card'><h3>System Status</h3><div id='status'>Loading...</div><button onclick='fetchStatus()'>Refresh</button></div>"
    "<div class='card'><h3>Controls</h3><button onclick=\"doAction('restart')\">Reboot Device</button><button onclick=\"doAction('wwd_resume')\">Start WWD</button><button onclick=\"doAction('wwd_stop')\">Stop WWD</button><button onclick=\"doAction('led_test')\">LED Test</button></div>"
    "<div class='card'><h3>OTA Update</h3><input type='text' id='otaUrl' placeholder='http://192.168.1.x:8000/firmware.bin'><br><button onclick='startOta()'>Start Update</button></div>"
    "<div class='card'><h3>Diagnostics</h3><a href='/webserial'><button>View Real-time Logs</button></a></div>"
    "<script>"
    "function fetchStatus(){fetch('/api/status').then(r=>r.json()).then(j=>{document.getElementById('status').innerText='IP: '+j.ip+' | Uptime: '+j.uptime+'s | WWD Active: '+(j.wwd?'Yes':'No')})}"
    "function doAction(cmd){fetch('/api/action',{method:'POST',body:'cmd='+cmd})}"
    "function startOta(){const url=document.getElementById('otaUrl').value; if(!url){alert('URL is empty');return;} if(confirm('Start OTA update? Device will reboot.')){fetch('/api/ota',{method:'POST',body:'url='+url}).then(r=>r.json()).then(j=>alert(j.ok?'Update started! Check logs.':'Failed to start update'))}}"
    "fetchStatus();setInterval(fetchStatus, 5000);"
    "</script></body></html>";

static const char *webserial_html =
    "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>body{font-family:sans-serif;margin:16px}button{padding:8px 14px;margin:4px}</style>"
    "</head><body><h2>System Logs</h2>"
    "<button onclick='location.reload()'>Refresh</button>"
    "<button onclick='clearLogs()'>Clear</button>"
    " <a href='/'><button>Back to Dashboard</button></a><hr>"
    "<pre id='c' style='height:70vh;overflow:auto;border:1px solid #ddd;padding:8px'></pre>"
    "<script>"
    "const logEl=document.getElementById('c');"
    "let lastSeq=0;"
    "function poll(){"
    "fetch('/webserial/logs?since='+lastSeq,{cache:'no-store'}).then(r=>{"
    "const reset=r.headers.get('X-Log-Reset')==='1';"
    "const seq=parseInt(r.headers.get('X-Log-Seq')||'0');"
    "return r.text().then(t=>{"
    "if(reset){logEl.innerText=t;}else{logEl.innerText+=t;}"
    "if(logEl.innerText.length>20000){logEl.innerText=logEl.innerText.slice(-20000);}"
    "logEl.scrollTop=logEl.scrollHeight;"
    "if(seq>0){lastSeq=seq;}"
    "});"
    "});"
    "}"
    "function clearLogs(){fetch('/webserial/clear').then(()=>{logEl.innerText='';lastSeq=0;});}"
    "poll();setInterval(poll,1000);"
    "</script></body></html>";

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
    if (len > 0) {
        if (len >= (int)sizeof(message)) {
            len = sizeof(message) - 1;
            message[len] = '\0';
        }
        if (log_mutex && xSemaphoreTake(log_mutex, 0)) {
            if (log_buffer_pos + len >= LOG_BUFFER_SIZE) {
                size_t overflow = log_buffer_pos + len - (LOG_BUFFER_SIZE - 1);
                if (overflow > log_buffer_pos) {
                    overflow = log_buffer_pos;
                }
                if (overflow > 0 && overflow < log_buffer_pos) {
                    memmove(log_buffer, log_buffer + overflow, log_buffer_pos - overflow);
                }
                log_buffer_pos -= overflow;
                log_base_seq += (uint32_t)overflow;
            }
            memcpy(log_buffer + log_buffer_pos, message, len);
            log_buffer_pos += (size_t)len;
            log_buffer[log_buffer_pos] = '\0';
            log_seq += (uint32_t)len;
            xSemaphoreGive(log_mutex);
        }
    }
    return ret;
}

static esp_err_t logs_handler(httpd_req_t *req) {
    client_count++;
    char query[64] = {0};
    char since_str[16] = {0};
    uint32_t since = 0;
    bool have_since = false;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "since", since_str, sizeof(since_str)) == ESP_OK) {
            since = (uint32_t)strtoul(since_str, NULL, 10);
            have_since = true;
        }
    }

    char *payload = NULL;
    size_t payload_len = 0;
    bool reset = false;
    uint32_t seq = 0;
    uint32_t base = 0;

    if (log_mutex && xSemaphoreTake(log_mutex, portMAX_DELAY)) {
        seq = log_seq;
        base = log_base_seq;

        if (!have_since) {
            payload_len = log_buffer_pos;
            payload = (char *)malloc(payload_len + 1);
            if (payload) {
                memcpy(payload, log_buffer, payload_len);
                payload[payload_len] = '\0';
            }
        } else {
            if (since < base) {
                reset = true;
                payload_len = log_buffer_pos;
                payload = (char *)malloc(payload_len + 1);
                if (payload) {
                    memcpy(payload, log_buffer, payload_len);
                    payload[payload_len] = '\0';
                }
            } else if (since <= seq) {
                size_t offset = (size_t)(since - base);
                if (offset > log_buffer_pos) {
                    offset = log_buffer_pos;
                }
                payload_len = log_buffer_pos - offset;
                payload = (char *)malloc(payload_len + 1);
                if (payload) {
                    memcpy(payload, log_buffer + offset, payload_len);
                    payload[payload_len] = '\0';
                }
            }
        }
        xSemaphoreGive(log_mutex);
    }

    httpd_resp_set_type(req, "text/plain");
    char header[16];
    snprintf(header, sizeof(header), "%u", seq);
    httpd_resp_set_hdr(req, "X-Log-Seq", header);
    snprintf(header, sizeof(header), "%u", base);
    httpd_resp_set_hdr(req, "X-Log-Base", header);
    if (reset) {
        httpd_resp_set_hdr(req, "X-Log-Reset", "1");
    }

    if (payload) {
        httpd_resp_send(req, payload, payload_len);
        free(payload);
    } else {
        httpd_resp_send(req, "", 0);
    }
    return ESP_OK;
}

static esp_err_t clear_handler(httpd_req_t *req) {
    if (log_mutex && xSemaphoreTake(log_mutex, portMAX_DELAY)) {
        log_buffer_pos = 0;
        log_buffer[0] = 0;
        log_base_seq = log_seq;
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
            else if (strcmp(cmd, "led_test") == 0) led_status_test_pattern();
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", 10);
}

static esp_err_t api_config_handler(httpd_req_t *req) {
    httpd_resp_send(req, "{}", 2);
    return ESP_OK;
}

static esp_err_t api_ota_handler(httpd_req_t *req) {
    char body[256];
    if (recv_body(req, body, sizeof(body)) == ESP_OK) {
        char url[192];
        if (form_get_param(body, "url", url, sizeof(url))) {
            ESP_LOGI(TAG, "OTA Requested via Web: %s", url);
            esp_err_t err = ota_update_start(url);
            httpd_resp_set_type(req, "application/json");
            if (err == ESP_OK) {
                return httpd_resp_send(req, "{\"ok\":true}", 10);
            }
            ESP_LOGE(TAG, "OTA start failed: %s", esp_err_to_name(err));
            return httpd_resp_send(req, "{\"ok\":false}", 11);
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing URL");
    return ESP_FAIL;
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
    config.max_open_sockets = 5; // Increased for better stability
    config.max_req_hdr_len = 8192;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uris[] = {
            {"/", HTTP_GET, dashboard_handler, NULL},
            {"/api/status", HTTP_GET, api_status_handler, NULL},
            {"/api/action", HTTP_POST, api_action_handler, NULL},
            {"/api/config", HTTP_POST, api_config_handler, NULL},
            {"/api/ota", HTTP_POST, api_ota_handler, NULL},
            {"/webserial", HTTP_GET, webserial_page_handler, NULL},
            {"/webserial/logs", HTTP_GET, logs_handler, NULL},
            {"/webserial/clear", HTTP_GET, clear_handler, NULL}
        };
        for (int i=0; i<sizeof(uris)/sizeof(uris[0]); i++) {
            httpd_register_uri_handler(server, &uris[i]);
        }
        original_log_func = esp_log_set_vprintf(webserial_log_func);
        server_running = true;
        ESP_LOGI(TAG, "Web Dashboard with OTA Support Started");
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
