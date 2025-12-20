/**
 * Home Assistant Client Implementation
 * WebSocket client for HA Assist Pipeline
 */

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include <stdlib.h> 
#include <string.h>
#include <ctype.h> 

#include "audio_capture.h"
#include "config.h" // For fallback/defaults if needed
#include "ha_client.h"
#include "oled_status.h"

static const char *TAG = "ha_client";

static esp_websocket_client_handle_t ws_client = NULL;
static bool ws_connected = false;
static bool ws_authenticated = false;
static int message_id = 1;

// Internal Config Storage
static ha_client_config_t client_config;
static char config_hostname[64];
static char config_token[512]; 

// Callbacks
static ha_conversation_callback_t conversation_callback = NULL;
static ha_tts_audio_callback_t tts_audio_callback = NULL;
static ha_pipeline_error_callback_t error_callback = NULL;
static ha_intent_callback_t intent_callback = NULL;
static ha_stt_callback_t stt_callback = NULL;

static int stt_binary_handler_id = -1;
static int last_run_message_id = -1;
static bool timer_started_this_conversation = false;
static bool speech_text_sent_this_run = false;

static uint8_t *audio_frame_buf = NULL;
static size_t audio_frame_buf_cap = 0;

static EventGroupHandle_t ha_event_group;
#define HA_CONNECTED_BIT BIT0
#define HA_AUTHENTICATED_BIT BIT1
#define HA_AUDIO_READY_BIT BIT2

#define HA_SEND_TEXT_TIMEOUT_MS 2000
#define HA_SEND_AUDIO_TIMEOUT_MS 2000

// Forward declarations
static void download_tts_audio(const char *url);
static bool ha_find_stt_handler_id(const cJSON *node, int depth, int *out_id);
static void ha_clear_audio_ready(void);
static void ha_set_audio_ready(int handler_id, const char *source);
static const char *ha_extract_intent_json(const cJSON *data_obj);
static bool ha_intent_name_is_timer(const char *intent_name);
static const char *ha_extract_stt_text(const cJSON *data_obj);

static void trim_ascii_whitespace_inplace(char *s) {
  if (s == NULL) return;

  char *start = s;
  while (*start && isspace((unsigned char)*start)) start++;

  size_t len = strlen(start);
  while (len > 0 && isspace((unsigned char)start[len - 1])) {
    start[--len] = '\0';
  }

  if (start != s) {
    memmove(s, start, len + 1);
  }
}

static const char *ha_extract_response_speech_plain_speech(const cJSON *data_obj) {
  if (!data_obj) return NULL;

  const cJSON *response = cJSON_GetObjectItemCaseSensitive((cJSON *)data_obj, "response");
  if (!response) {
    const cJSON *intent_output = cJSON_GetObjectItemCaseSensitive((cJSON *)data_obj, "intent_output");
    if (intent_output) {
      response = cJSON_GetObjectItemCaseSensitive((cJSON *)intent_output, "response");
    }
  }
  if (!response) return NULL;

  const cJSON *speech = cJSON_GetObjectItemCaseSensitive((cJSON *)response, "speech");
  if (!speech) return NULL;

  const cJSON *plain = cJSON_GetObjectItemCaseSensitive((cJSON *)speech, "plain");
  if (!plain) return NULL;

  const cJSON *speech_txt = cJSON_GetObjectItemCaseSensitive((cJSON *)plain, "speech");
  if (speech_txt && cJSON_IsString(speech_txt) && speech_txt->valuestring &&
      speech_txt->valuestring[0] != '\0') {
    return speech_txt->valuestring;
  }

  return NULL;
}

// ... Helper functions (ha_parse_int_item, etc.) ...
static bool ha_parse_int_item(const cJSON *item, int *out_id) {
  if (item == NULL || out_id == NULL) return false;
  if (cJSON_IsNumber(item)) { *out_id = item->valueint; return true; }
  if (cJSON_IsString(item) && item->valuestring) {
    char *end = NULL;
    long v = strtol(item->valuestring, &end, 10);
    if (end != item->valuestring) { *out_id = (int)v; return true; }
  }
  return false;
}

static bool ha_find_stt_handler_id(const cJSON *node, int depth, int *out_id) {
  if (node == NULL || out_id == NULL || depth <= 0) return false;
  if (cJSON_IsObject(node)) {
    for (const cJSON *child = node->child; child != NULL; child = child->next) {
      if (child->string && strcmp(child->string, "stt_binary_handler_id") == 0) {
        if (ha_parse_int_item(child, out_id)) return true;
      }
      if (ha_find_stt_handler_id(child, depth - 1, out_id)) return true;
    }
  } else if (cJSON_IsArray(node)) {
    for (const cJSON *child = node->child; child != NULL; child = child->next) {
      if (ha_find_stt_handler_id(child, depth - 1, out_id)) return true;
    }
  }
  return false;
}

static const char *ha_extract_intent_json(const cJSON *data_obj) {
  static char intent_buf[512];
  const cJSON *intent = NULL;
  const cJSON *intent_output = NULL;

  if (!data_obj) return NULL;

  intent = cJSON_GetObjectItemCaseSensitive((cJSON *)data_obj, "intent");
  if (!intent) {
    intent_output = cJSON_GetObjectItemCaseSensitive((cJSON *)data_obj, "intent_output");
    if (intent_output) {
      intent = cJSON_GetObjectItemCaseSensitive((cJSON *)intent_output, "intent");
    }
  }
  if (!intent) return NULL;

  char *intent_str = cJSON_PrintUnformatted((cJSON *)intent);
  if (!intent_str) return NULL;

  size_t len = strlen(intent_str);
  if (len >= sizeof(intent_buf)) len = sizeof(intent_buf) - 1;
  memcpy(intent_buf, intent_str, len);
  intent_buf[len] = '\0';
  free(intent_str);
  return intent_buf;
}

static bool ha_intent_name_is_timer(const char *intent_name) {
  if (!intent_name) return false;
  return (strstr(intent_name, "timer") || strstr(intent_name, "Timer"));
}

static const char *ha_extract_stt_text(const cJSON *data_obj) {
  const cJSON *stt_out = NULL;
  const cJSON *text = NULL;

  if (!data_obj) return NULL;

  stt_out = cJSON_GetObjectItemCaseSensitive((cJSON *)data_obj, "stt_output");
  if (!stt_out) {
    stt_out = cJSON_GetObjectItemCaseSensitive((cJSON *)data_obj, "stt");
  }
  if (!stt_out) return NULL;

  text = cJSON_GetObjectItemCaseSensitive((cJSON *)stt_out, "text");
  if (text && cJSON_IsString(text) && text->valuestring && text->valuestring[0] != '\0') {
    return text->valuestring;
  }

  return NULL;
}

static void ha_clear_audio_ready(void) {
  stt_binary_handler_id = -1;
  if (ha_event_group) xEventGroupClearBits(ha_event_group, HA_AUDIO_READY_BIT);
}

static void ha_set_audio_ready(int handler_id, const char *source) {
  if (handler_id < 0 || handler_id > 255) {
    ESP_LOGE(TAG, "Invalid stt_binary_handler_id=%d (%s)", handler_id, source ? source : "unknown");
    return;
  }
  stt_binary_handler_id = handler_id;
  if (ha_event_group) xEventGroupSetBits(ha_event_group, HA_AUDIO_READY_BIT);
  ESP_LOGI(TAG, "STT binary handler ID: %d (%s)", stt_binary_handler_id, source ? source : "unknown");
  oled_status_set_last_event("stt-bin");
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

  switch (event_id) {
  case WEBSOCKET_EVENT_CONNECTED:
    ESP_LOGI(TAG, "WebSocket connected");
    ws_connected = true;
    xEventGroupSetBits(ha_event_group, HA_CONNECTED_BIT);
    oled_status_set_last_event("ws-up");

    // Send authentication
    char auth_msg[1024];
    snprintf(auth_msg, sizeof(auth_msg), "{\"type\":\"auth\",\"access_token\":\"%s\"}", client_config.access_token);
    int auth_ret = esp_websocket_client_send_text(ws_client, auth_msg, strlen(auth_msg), pdMS_TO_TICKS(HA_SEND_TEXT_TIMEOUT_MS));
    if (auth_ret < 0) ESP_LOGE(TAG, "Failed to send auth");
    else ESP_LOGI(TAG, "Sent auth token");
    break;

  case WEBSOCKET_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "WebSocket disconnected");
    ws_connected = false;
    ws_authenticated = false;
    ha_clear_audio_ready();
    xEventGroupClearBits(ha_event_group, HA_CONNECTED_BIT | HA_AUTHENTICATED_BIT | HA_AUDIO_READY_BIT);
    oled_status_set_ha_connected(false);
    oled_status_set_last_event("ws-down");
    break;

  case WEBSOCKET_EVENT_DATA:
    if (data->op_code == 0x02 || data->op_code == 0x09 || data->op_code == 0x0A) break;
    if (data->data_len <= 0 || !data->data_ptr) break;

    // Parse JSON
    cJSON *json = cJSON_ParseWithLength((char *)data->data_ptr, data->data_len);
    if (!json) { ESP_LOGE(TAG, "Failed to parse JSON"); break; }

    cJSON *type = cJSON_GetObjectItem(json, "type");
    if (!type || !cJSON_IsString(type) || !type->valuestring) {
        ESP_LOGW(TAG, "JSON missing type or not string");
        cJSON_Delete(json);
        break;
    }

    if (strcmp(type->valuestring, "auth_ok") == 0) {
        ESP_LOGI(TAG, "Auth successful");
        ws_authenticated = true;
        xEventGroupSetBits(ha_event_group, HA_AUTHENTICATED_BIT);
        oled_status_set_ha_connected(true);
        oled_status_set_last_event("auth-ok");
    } else if (type && strcmp(type->valuestring, "auth_invalid") == 0) {
        ESP_LOGE(TAG, "Auth failed");
        ws_authenticated = false;
        oled_status_set_ha_connected(false);
        oled_status_set_last_event("auth-bad");
    } else if (type && strcmp(type->valuestring, "event") == 0) {
        cJSON *event = cJSON_GetObjectItem(json, "event");
        if (event) {
            cJSON *evt_type = cJSON_GetObjectItem(event, "type");
            cJSON *data_obj = cJSON_GetObjectItem(event, "data");
            
            if (evt_type && evt_type->valuestring) {
                if (strcmp(evt_type->valuestring, "run-start") == 0) {
                    timer_started_this_conversation = false;
                    speech_text_sent_this_run = false;
                    int hid = -1;
                    if (data_obj && ha_find_stt_handler_id(data_obj, 6, &hid)) {
                        ha_set_audio_ready(hid, "run-start");
                    }
                } else if (strcmp(evt_type->valuestring, "intent-end") == 0) {
                    const cJSON *intent = NULL;
                    const cJSON *intent_output = NULL;
                    const cJSON *intent_name = NULL;
                    const char *intent_data = NULL;

                    if (data_obj) {
                        intent = cJSON_GetObjectItemCaseSensitive((cJSON *)data_obj, "intent");
                        if (!intent) {
                            intent_output = cJSON_GetObjectItemCaseSensitive((cJSON *)data_obj, "intent_output");
                            if (intent_output) {
                                intent = cJSON_GetObjectItemCaseSensitive((cJSON *)intent_output, "intent");
                            }
                        }
                        if (intent) {
                            intent_name = cJSON_GetObjectItemCaseSensitive((cJSON *)intent, "name");
                        }
                        intent_data = ha_extract_intent_json(data_obj);
                    }

                    if (intent_name && cJSON_IsString(intent_name) && intent_name->valuestring && intent_callback) {
                        if (ha_intent_name_is_timer(intent_name->valuestring)) {
                            timer_started_this_conversation = true;
                        }
                        intent_callback(intent_name->valuestring, intent_data, NULL);
                    }

                    const char *speech = ha_extract_response_speech_plain_speech(data_obj);
                    if (speech && conversation_callback) {
                        conversation_callback(speech, NULL);
                        speech_text_sent_this_run = true;
                    }
                } else if (strcmp(evt_type->valuestring, "stt-end") == 0) {
                    const char *stt_text = ha_extract_stt_text(data_obj);
                    if (stt_text && stt_callback) {
                        stt_callback(stt_text, NULL);
                    }
                } else if (strcmp(evt_type->valuestring, "tts-end") == 0) {
                    if (timer_started_this_conversation) {
                        ESP_LOGI(TAG, "Skipping TTS (timer started)");
                    } else if (data_obj) {
                        cJSON *tts_out = cJSON_GetObjectItem(data_obj, "tts_output");
                        if (tts_out) {
                            cJSON *text = cJSON_GetObjectItem(tts_out, "text");
                            if (!speech_text_sent_this_run && text && text->valuestring && conversation_callback) {
                                conversation_callback(text->valuestring, NULL);
                            }
                            cJSON *url = cJSON_GetObjectItem(tts_out, "url");
                            if (url && url->valuestring) {
                                download_tts_audio(url->valuestring);
                            }
                        }
                    }
                } else if (strcmp(evt_type->valuestring, "run-end") == 0) {
                    if (timer_started_this_conversation && conversation_callback) {
                        conversation_callback("", NULL);
                    }
                    ha_clear_audio_ready();
                } else if (strcmp(evt_type->valuestring, "error") == 0) {
                    // Error handling...
                    if (error_callback) error_callback("error", "Pipeline Error");
                    audio_capture_stop_wait(500);
                    ha_clear_audio_ready();
                }
                // ... (Other event types: intent-end, stt-end - simplified for now, logic remains similar)
            }
        }
    } else if (type && strcmp(type->valuestring, "result") == 0) {
        // Result handling (late handler_id)
        cJSON *msg_id = cJSON_GetObjectItem(json, "id");
        cJSON *res = cJSON_GetObjectItem(json, "result");
        if (msg_id && cJSON_IsNumber(msg_id) && res && (int)msg_id->valuedouble == last_run_message_id && !ha_client_is_audio_ready()) {
             int hid = -1;
             if (ha_find_stt_handler_id(res, 6, &hid)) ha_set_audio_ready(hid, "result");
        }
    }
    cJSON_Delete(json);
    break;
  default: break;
  }
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA && tts_audio_callback && evt->data_len > 0) {
      tts_audio_callback((const uint8_t *)evt->data, evt->data_len);
  }
  return ESP_OK;
}

static void download_tts_audio(const char *url) {
  if (!url || !strlen(url)) return;

  char full_url[1024];
  // Construct URL using configured hostname
  if (client_config.use_ssl) {
      snprintf(full_url, sizeof(full_url), "https://%s:%d%s", client_config.hostname, client_config.port, url);
  } else {
      snprintf(full_url, sizeof(full_url), "http://%s:%d%s", client_config.hostname, client_config.port, url);
  }
  ESP_LOGI(TAG, "Downloading TTS: %s", full_url);

  esp_http_client_config_t config = {
      .url = full_url,
      .event_handler = http_event_handler,
      .timeout_ms = 10000,
  };
  if (client_config.use_ssl) config.skip_cert_common_name_check = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
      if (tts_audio_callback) tts_audio_callback(NULL, 0);
      return;
  }

  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK && esp_http_client_get_status_code(client) == 200) {
      if (tts_audio_callback) tts_audio_callback(NULL, 0); // End
  } else {
      ESP_LOGE(TAG, "TTS download failed: %s", esp_err_to_name(err));
      if (tts_audio_callback) tts_audio_callback(NULL, 0);
  }
  esp_http_client_cleanup(client);
}

static esp_err_t init_mdns(void) {
  if (mdns_init() != ESP_OK) return ESP_FAIL;
  mdns_hostname_set("esp32-p4-voice-assistant");
  return ESP_OK;
}

esp_err_t ha_client_init(const ha_client_config_t *config) {
  if (!config) return ESP_ERR_INVALID_ARG;

  // Copy config
  strncpy(config_hostname, config->hostname, sizeof(config_hostname)-1);
  config_hostname[sizeof(config_hostname)-1] = '\0';
  strncpy(config_token, config->access_token, sizeof(config_token)-1);
  config_token[sizeof(config_token)-1] = '\0';
  trim_ascii_whitespace_inplace(config_hostname);
  trim_ascii_whitespace_inplace(config_token);
  client_config.hostname = config_hostname;
  client_config.access_token = config_token;
  client_config.port = config->port;
  client_config.use_ssl = config->use_ssl;

  ha_client_stop();
  ha_event_group = xEventGroupCreate();
  init_mdns();

  char ws_uri[256];
  snprintf(ws_uri, sizeof(ws_uri), "%s://%s:%d%s", 
           client_config.use_ssl ? "wss" : "ws", 
           client_config.hostname, client_config.port, HA_WEBSOCKET_PATH);
  
  esp_websocket_client_config_t ws_cfg = {
      .uri = ws_uri,
      .task_stack = 8192,
      .buffer_size = 4096,
      .disable_auto_reconnect = false,
      .network_timeout_ms = 10000,
  };
  if (client_config.use_ssl) {
      ws_cfg.transport = WEBSOCKET_TRANSPORT_OVER_SSL;
      ws_cfg.skip_cert_common_name_check = true;
      ws_cfg.use_global_ca_store = false;
  }

  ws_client = esp_websocket_client_init(&ws_cfg);
  if (!ws_client) return ESP_FAIL;

  esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
  if (esp_websocket_client_start(ws_client) != ESP_OK) {
      ha_client_stop();
      return ESP_FAIL;
  }

  xEventGroupWaitBits(ha_event_group, HA_AUTHENTICATED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
  return ha_client_is_connected() ? ESP_OK : ESP_ERR_TIMEOUT;
}

// ... (Rest of Public API: is_connected, request_tts, start_conversation, etc. - keep as is)
// For brevity, assume standard getters/senders are here. 
// I'll add the essential missing ones below.

bool ha_client_is_connected(void) { return ws_connected && ws_authenticated; }
bool ha_client_is_audio_ready(void) { return ha_client_is_connected() && stt_binary_handler_id >= 0; }
int ha_client_get_stt_binary_handler_id(void) { return stt_binary_handler_id; }

esp_err_t ha_client_send_text(const char *text) {
    return ha_client_request_tts(text);
}

esp_err_t ha_client_request_tts(const char *text) {
    if (!ha_client_is_connected()) return ESP_FAIL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", message_id++);
    cJSON_AddStringToObject(root, "type", "assist_pipeline/run");
    cJSON_AddStringToObject(root, "start_stage", "intent");
    cJSON_AddStringToObject(root, "end_stage", "tts");
    cJSON *input = cJSON_CreateObject();
    cJSON_AddStringToObject(input, "text", text);
    cJSON_AddItemToObject(root, "input", input);
    
    char *str = cJSON_PrintUnformatted(root);
    esp_websocket_client_send_text(ws_client, str, strlen(str), pdMS_TO_TICKS(2000));
    free(str);
    cJSON_Delete(root);
    return ESP_OK;
}

char* ha_client_start_conversation(void) {
    if (!ha_client_is_connected()) return NULL;
    ha_clear_audio_ready();
    
    cJSON *root = cJSON_CreateObject();
    last_run_message_id = message_id++;
    cJSON_AddNumberToObject(root, "id", last_run_message_id);
    cJSON_AddStringToObject(root, "type", "assist_pipeline/run");
    cJSON_AddStringToObject(root, "start_stage", "stt");
    cJSON_AddStringToObject(root, "end_stage", "tts");
    cJSON *input = cJSON_CreateObject();
    cJSON_AddNumberToObject(input, "sample_rate", 16000);
    cJSON_AddItemToObject(root, "input", input);
    
    char *str = cJSON_PrintUnformatted(root);
    esp_websocket_client_send_text(ws_client, str, strlen(str), pdMS_TO_TICKS(2000));
    free(str);
    cJSON_Delete(root);
    
    char *hid = malloc(32);
    snprintf(hid, 32, "run_%d", last_run_message_id);
    return hid;
}

esp_err_t ha_client_stream_audio(const uint8_t *audio_data, size_t length, const char *conversation_id) {
    if (!ha_client_is_connected() || stt_binary_handler_id < 0) return ESP_FAIL;
    
    size_t needed = 1 + length;
    if (!audio_frame_buf || audio_frame_buf_cap < needed) {
        uint8_t *new_buf = realloc(audio_frame_buf, needed);
        if (!new_buf) {
            ESP_LOGE(TAG, "Failed to realloc audio buffer (OOM)");
            return ESP_ERR_NO_MEM;
        }
        audio_frame_buf = new_buf;
        audio_frame_buf_cap = needed;
    }
    audio_frame_buf[0] = (uint8_t)stt_binary_handler_id;
    memcpy(audio_frame_buf+1, audio_data, length);
    
    esp_websocket_client_send_bin(ws_client, (const char*)audio_frame_buf, needed, pdMS_TO_TICKS(HA_SEND_AUDIO_TIMEOUT_MS));
    return ESP_OK;
}

esp_err_t ha_client_end_audio_stream(void) {
    if (!ha_client_is_connected() || stt_binary_handler_id < 0) return ESP_FAIL;
    uint8_t b = (uint8_t)stt_binary_handler_id;
    esp_websocket_client_send_bin(ws_client, (const char*)&b, 1, pdMS_TO_TICKS(HA_SEND_AUDIO_TIMEOUT_MS));
    ha_clear_audio_ready();
    return ESP_OK;
}

void ha_client_register_conversation_callback(ha_conversation_callback_t cb) { conversation_callback = cb; }
void ha_client_register_tts_audio_callback(ha_tts_audio_callback_t cb) { tts_audio_callback = cb; }
void ha_client_register_error_callback(ha_pipeline_error_callback_t cb) { error_callback = cb; }
void ha_client_register_intent_callback(ha_intent_callback_t cb) { intent_callback = cb; }
void ha_client_register_stt_callback(ha_stt_callback_t cb) { stt_callback = cb; }

void ha_client_stop(void) {
    if (ws_client) {
        esp_websocket_client_stop(ws_client);
        esp_websocket_client_destroy(ws_client);
        ws_client = NULL;
    }
    if (ha_event_group) vEventGroupDelete(ha_event_group);
    ha_event_group = NULL;
    ws_connected = false;
    ws_authenticated = false;
}
