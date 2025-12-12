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


#include "config.h"
#include "connection_manager.h"
#include "ha_client.h"


static const char *TAG = "ha_client";

static esp_websocket_client_handle_t ws_client = NULL;
static bool ws_connected = false;
static bool ws_authenticated = false;
static int message_id = 1;

// Conversation callback
static ha_conversation_callback_t conversation_callback = NULL;

// TTS audio callback
static ha_tts_audio_callback_t tts_audio_callback = NULL;

// Pipeline error callback
static ha_pipeline_error_callback_t error_callback = NULL;

// Intent callback
static ha_intent_callback_t intent_callback = NULL;

// STT binary handler ID (received from run-start event)
static int stt_binary_handler_id = -1;

// Flag to track if a timer was successfully started (to skip negative HA response)
static bool timer_started_this_conversation = false;

// Event group for tracking connection state
static EventGroupHandle_t ha_event_group;
#define HA_CONNECTED_BIT BIT0
#define HA_AUTHENTICATED_BIT BIT1

// Forward declarations
static void download_tts_audio(const char *url);

// External function to stop audio capture (defined in audio_capture.c)
extern void audio_capture_stop(void);

/**
 * Resolve mDNS hostname to IP address
 * Currently unused - WebSocket client handles hostname resolution internally
 */
#if 0
static esp_err_t resolve_mdns_host(const char *hostname, char *ip_str, size_t ip_str_len)
{
    ESP_LOGI(TAG, "Resolving mDNS hostname: %s", hostname);

    // Query mDNS for the hostname
    struct esp_ip4_addr addr;
    addr.addr = 0;

    esp_err_t err = mdns_query_a(hostname, 2000, &addr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS query failed for %s, using fallback IP", hostname);
        return ESP_FAIL;
    }

    snprintf(ip_str, ip_str_len, IPSTR, IP2STR(&addr));
    ESP_LOGI(TAG, "Resolved %s to %s", hostname, ip_str);
    return ESP_OK;
}
#endif

/**
 * WebSocket event handler
 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data) {
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

  switch (event_id) {
  case WEBSOCKET_EVENT_CONNECTED:
    ESP_LOGI(TAG, "WebSocket connected to Home Assistant");
    ws_connected = true;
    xEventGroupSetBits(ha_event_group, HA_CONNECTED_BIT);
    connection_manager_update_state(CONN_TYPE_HA_WEBSOCKET,
                                    CONN_STATE_CONNECTED);

    // Send authentication message
    char auth_msg[512];
    snprintf(auth_msg, sizeof(auth_msg),
             "{\"type\":\"auth\",\"access_token\":\"%s\"}", HA_TOKEN);
    esp_websocket_client_send_text(ws_client, auth_msg, strlen(auth_msg),
                                   portMAX_DELAY);
    ESP_LOGI(TAG, "Sent authentication token");
    break;

  case WEBSOCKET_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "WebSocket disconnected from Home Assistant");
    ws_connected = false;
    ws_authenticated = false;
    stt_binary_handler_id = -1;
    xEventGroupClearBits(ha_event_group,
                         HA_CONNECTED_BIT | HA_AUTHENTICATED_BIT);
    connection_manager_update_state(CONN_TYPE_HA_WEBSOCKET,
                                    CONN_STATE_DISCONNECTED);
    break;

  case WEBSOCKET_EVENT_DATA:
    // Binary frames are not used for TTS (we download via HTTP instead)
    if (data->op_code == 0x02) {
      ESP_LOGD(TAG, "Received binary frame: %d bytes (unused)", data->data_len);
      break;
    }

    // Text frame - parse JSON
    ESP_LOGI(TAG, "WebSocket received data: %.*s", data->data_len,
             (char *)data->data_ptr);

    // Parse JSON response
    if (data->data_len > 0) {
      cJSON *json =
          cJSON_ParseWithLength((char *)data->data_ptr, data->data_len);
      if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        break;
      }

      cJSON *type = cJSON_GetObjectItem(json, "type");

      if (type && strcmp(type->valuestring, "auth_ok") == 0) {
        ESP_LOGI(TAG, "Home Assistant authentication successful!");
        ws_authenticated = true;
        xEventGroupSetBits(ha_event_group, HA_AUTHENTICATED_BIT);
      } else if (type && strcmp(type->valuestring, "auth_invalid") == 0) {
        ESP_LOGE(TAG, "Home Assistant authentication failed - check token!");
        ws_authenticated = false;
      } else if (type && strcmp(type->valuestring, "event") == 0) {
        // Assist Pipeline events
        cJSON *event = cJSON_GetObjectItem(json, "event");
        if (event) {
          cJSON *event_type = cJSON_GetObjectItem(event, "type");
          if (event_type && event_type->valuestring) {
            cJSON *data_obj = cJSON_GetObjectItem(event, "data");

            if (strcmp(event_type->valuestring, "run-start") == 0) {
              ESP_LOGI(TAG, "Pipeline started");

              // Reset timer flag for new conversation
              timer_started_this_conversation = false;

              // Extract STT binary handler ID for audio streaming
              if (data_obj) {
                cJSON *runner_data =
                    cJSON_GetObjectItem(data_obj, "runner_data");
                if (runner_data) {
                  cJSON *stt_handler =
                      cJSON_GetObjectItem(runner_data, "stt_binary_handler_id");
                  if (stt_handler && cJSON_IsNumber(stt_handler)) {
                    stt_binary_handler_id = stt_handler->valueint;
                    ESP_LOGI(TAG, "STT binary handler ID: %d",
                             stt_binary_handler_id);
                  }
                }
              }
            } else if (strcmp(event_type->valuestring, "stt-start") == 0) {
              ESP_LOGI(TAG, "Speech-to-Text started");
            } else if (strcmp(event_type->valuestring, "stt-end") == 0) {
              if (data_obj) {
                cJSON *stt_output = cJSON_GetObjectItem(data_obj, "stt_output");
                if (stt_output) {
                  cJSON *text = cJSON_GetObjectItem(stt_output, "text");
                  if (text && text->valuestring) {
                    ESP_LOGI(TAG, "STT Transcript: '%s'", text->valuestring);

                    // Parse timer commands from STT text
                    if (intent_callback) {
                      const char *transcript = text->valuestring;
                      char transcript_lower[256];

                      // Convert to lowercase for matching
                      strncpy(transcript_lower, transcript, sizeof(transcript_lower) - 1);
                      transcript_lower[sizeof(transcript_lower) - 1] = '\0';
                      for (int i = 0; transcript_lower[i]; i++) {
                        transcript_lower[i] = tolower((unsigned char)transcript_lower[i]);
                      }

                      // Check for timer keywords (Latin + Cyrillic)
                      if (strstr(transcript_lower, "timer") != NULL ||
                          strstr(transcript_lower, "tajmer") != NULL ||
                          strstr(transcript, "тајмер") != NULL ||
                          strstr(transcript, "Тајмер") != NULL) {

                        int duration_min = 0, duration_sec = 0;
                        bool timer_found = false;

                        // Helper: Parse Croatian text numbers (1-60)
                        const char *text_numbers[] = {
                          "jedan", "jedna", "jednu", "dva", "dvije", "tri", "četiri", "pet",
                          "šest", "sedam", "osam", "devet", "deset",
                          "jedanaest", "dvanaest", "trinaest", "četrnaest", "petnaest",
                          "šesnaest", "sedamnaest", "osamnaest", "devetnaest", "dvadeset",
                          "trideset", "četrdeset", "pedeset", "šezdeset"
                        };
                        const int text_values[] = {
                          1, 1, 1, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                          11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 30, 40, 50, 60
                        };

                        // Try to parse different timer formats
                        // First check for text-based seconds
                        if (strstr(transcript_lower, "sekund")) {
                          for (int i = 0; i < sizeof(text_values)/sizeof(text_values[0]); i++) {
                            if (strstr(transcript_lower, text_numbers[i])) {
                              duration_sec = text_values[i];
                              timer_found = true;
                              break;
                            }
                          }
                        }
                        // Then check for text-based minutes (only if seconds not found)
                        if (!timer_found && (strstr(transcript_lower, "minut"))) {
                          for (int i = 0; i < sizeof(text_values)/sizeof(text_values[0]); i++) {
                            if (strstr(transcript_lower, text_numbers[i])) {
                              duration_min = text_values[i];
                              timer_found = true;
                              break;
                            }
                          }
                        }

                        // Fallback to numeric parsing if text-based parsing failed
                        // Check for numeric formats - try seconds FIRST (more specific)
                        if (!timer_found && (strstr(transcript_lower, "sekund") || strstr(transcript, "секунд"))) {
                          // Format: "X sekundi/sekunda/sekunde" (Latin or Cyrillic)
                          int num = 0;
                          if (sscanf(transcript, "%*s %*s %*s %d", &num) == 1 ||
                              sscanf(transcript, "%*s %*s %d", &num) == 1) {
                            duration_sec = num;
                            timer_found = true;
                          }
                        }
                        // Then try minutes
                        if (!timer_found && (strstr(transcript_lower, "minut") || strstr(transcript, "минут"))) {
                          // Format: "X minuta/minute/minuti" (Latin or Cyrillic)
                          int num = 0;
                          if (sscanf(transcript, "%*s %*s %*s %d", &num) == 1 ||
                              sscanf(transcript, "%*s %*s %d", &num) == 1) {
                            duration_min = num;
                            timer_found = true;
                          }
                        }

                        if (timer_found) {
                          int total_seconds = (duration_min * 60) + duration_sec;
                          if (total_seconds > 0) {
                            // Create synthetic intent data with proper name
                            char intent_data[256];
                            char timer_name[64];
                            if (duration_min > 0 && duration_sec > 0) {
                              snprintf(timer_name, sizeof(timer_name), "%dm %ds timer", duration_min, duration_sec);
                            } else if (duration_min > 0) {
                              snprintf(timer_name, sizeof(timer_name), "%d minute timer", duration_min);
                            } else {
                              snprintf(timer_name, sizeof(timer_name), "%d second timer", duration_sec);
                            }

                            snprintf(intent_data, sizeof(intent_data),
                                   "{\"targets\":[{\"type\":\"timer\",\"name\":\"%s\",\"duration\":%d}]}",
                                   timer_name, total_seconds);

                            ESP_LOGI(TAG, "🎯 Detected timer command: %d seconds", total_seconds);
                            intent_callback("timer", intent_data, NULL);

                            // Mark that timer was started to skip negative HA TTS
                            timer_started_this_conversation = true;
                          }
                        }
                      }
                    }
                  }
                }
              }
            } else if (strcmp(event_type->valuestring, "intent-start") == 0) {
              ESP_LOGI(TAG, "Intent recognition started");
            } else if (strcmp(event_type->valuestring, "intent-end") == 0) {
              if (data_obj) {
                cJSON *intent_output =
                    cJSON_GetObjectItem(data_obj, "intent_output");
                if (intent_output) {
                  cJSON *conversation_id =
                      cJSON_GetObjectItem(intent_output, "conversation_id");

                  // Extract intent response data
                  cJSON *response = cJSON_GetObjectItem(intent_output, "response");
                  if (response) {
                    cJSON *response_type = cJSON_GetObjectItem(response, "response_type");
                    cJSON *data = cJSON_GetObjectItem(response, "data");

                    // Check for action_done response (successful intent execution)
                    if (response_type && response_type->valuestring &&
                        strcmp(response_type->valuestring, "action_done") == 0 && data) {

                      // Extract intent name from targets
                      cJSON *targets = cJSON_GetObjectItem(data, "targets");
                      if (targets && cJSON_IsArray(targets) && cJSON_GetArraySize(targets) > 0) {
                        cJSON *target = cJSON_GetArrayItem(targets, 0);
                        cJSON *type = cJSON_GetObjectItem(target, "type");

                        if (type && type->valuestring) {
                          ESP_LOGI(TAG, "Intent action: %s", type->valuestring);

                          // Call intent callback with full data JSON
                          if (intent_callback) {
                            char *data_str = cJSON_PrintUnformatted(data);
                            if (data_str) {
                              intent_callback(type->valuestring, data_str,
                                            conversation_id && conversation_id->valuestring
                                                ? conversation_id->valuestring
                                                : NULL);
                              cJSON_free(data_str);
                            }
                          }
                        }
                      }
                    }
                  }

                  ESP_LOGI(TAG, "Intent matched (conv: %s)",
                           conversation_id && conversation_id->valuestring
                               ? conversation_id->valuestring
                               : "none");
                }
              }
            } else if (strcmp(event_type->valuestring, "tts-start") == 0) {
              ESP_LOGI(TAG, "Text-to-Speech started");
            } else if (strcmp(event_type->valuestring, "tts-end") == 0) {
              // Skip TTS if timer was successfully started (avoid "cannot set timer" message)
              if (timer_started_this_conversation) {
                ESP_LOGI(TAG, "Skipping HA TTS (timer already started with confirmation beep)");
              } else if (data_obj) {
                cJSON *tts_output = cJSON_GetObjectItem(data_obj, "tts_output");
                if (tts_output) {
                  // Get TTS URL for downloading audio
                  cJSON *url = cJSON_GetObjectItem(tts_output, "url");
                  if (url && url->valuestring) {
                    ESP_LOGI(TAG, "TTS URL: %s", url->valuestring);

                    // Download TTS audio file
                    download_tts_audio(url->valuestring);
                  }

                  // Also get text for conversation callback
                  cJSON *text = cJSON_GetObjectItem(tts_output, "text");
                  if (text && text->valuestring) {
                    ESP_LOGI(TAG, "TTS Response: '%s'", text->valuestring);

                    // Call conversation callback with TTS text
                    if (conversation_callback) {
                      conversation_callback(text->valuestring, NULL);
                    }
                  }
                }
              }
            } else if (strcmp(event_type->valuestring, "run-end") == 0) {
              ESP_LOGI(TAG, "Pipeline completed");

              // If timer was started and TTS was skipped, signal completion to resume wake word
              if (timer_started_this_conversation && conversation_callback) {
                ESP_LOGI(TAG, "Signaling pipeline completion (timer path)");
                conversation_callback("", NULL); // Empty string signals timer completion
              }

              // Reset handler ID for next run
              stt_binary_handler_id = -1;
            } else if (strcmp(event_type->valuestring, "error") == 0) {
              cJSON *code = cJSON_GetObjectItem(data_obj, "code");
              cJSON *message = cJSON_GetObjectItem(data_obj, "message");
              const char *error_code = code && code->valuestring ? code->valuestring : "unknown";
              const char *error_msg = message && message->valuestring ? message->valuestring : "";

              ESP_LOGE(TAG, "Pipeline error: %s - %s", error_code, error_msg);

              // Stop audio capture on pipeline error
              audio_capture_stop();
              ESP_LOGI(TAG, "Audio capture stopped due to pipeline error");

              // Reset handler ID
              stt_binary_handler_id = -1;

              // Call error callback if registered
              if (error_callback) {
                error_callback(error_code, error_msg);
              }
            }
          }
        }
      } else if (type && strcmp(type->valuestring, "result") == 0) {
        cJSON *success = cJSON_GetObjectItem(json, "success");
        if (success && cJSON_IsTrue(success)) {
          ESP_LOGI(TAG, "Command executed successfully");

          // Check for conversation response
          cJSON *result = cJSON_GetObjectItem(json, "result");
          if (result) {
            cJSON *response = cJSON_GetObjectItem(result, "response");
            if (response) {
              cJSON *speech = cJSON_GetObjectItem(response, "speech");
              if (speech) {
                cJSON *plain = cJSON_GetObjectItem(speech, "plain");
                if (plain) {
                  cJSON *speech_text = cJSON_GetObjectItem(plain, "speech");
                  if (speech_text && speech_text->valuestring) {
                    ESP_LOGI(TAG, "HA Response: %s", speech_text->valuestring);

                    // Call callback if registered
                    if (conversation_callback) {
                      cJSON *conversation_id =
                          cJSON_GetObjectItem(result, "conversation_id");
                      const char *conv_id =
                          conversation_id ? conversation_id->valuestring : NULL;
                      conversation_callback(speech_text->valuestring, conv_id);
                    }
                  }
                }
              }
            }
          }
        }
      }

      cJSON_Delete(json);
    }
    break;

  case WEBSOCKET_EVENT_ERROR:
    ESP_LOGE(TAG, "WebSocket error");
    break;

  default:
    break;
  }
}

/**
 * HTTP event handler for downloading TTS audio
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  switch (evt->event_id) {
  case HTTP_EVENT_ON_DATA:
    // Received audio data chunk
    if (tts_audio_callback && evt->data_len > 0) {
      tts_audio_callback((const uint8_t *)evt->data, evt->data_len);
    }
    break;
  default:
    break;
  }
  return ESP_OK;
}

/**
 * Download TTS audio file from Home Assistant
 */
static void download_tts_audio(const char *url) {
  if (url == NULL || strlen(url) == 0) {
    ESP_LOGW(TAG, "Invalid TTS URL");
    return;
  }

  // Build full URL - use HA_HOST (IP) instead of HA_HOSTNAME for compatibility
  char full_url[512];
  if (HA_USE_SSL) {
    snprintf(full_url, sizeof(full_url), "https://%s:%d%s", HA_HOST, HA_PORT,
             url);
  } else {
    snprintf(full_url, sizeof(full_url), "http://%s:%d%s", HA_HOST, HA_PORT,
             url);
  }

  ESP_LOGI(TAG, "Downloading TTS audio from: %s", full_url);

  esp_http_client_config_t config = {
      .url = full_url,
      .event_handler = http_event_handler,
      .timeout_ms = 10000,
  };

  if (HA_USE_SSL) {
    config.skip_cert_common_name_check = true;
  }

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    // Signal error to TTS player
    if (tts_audio_callback) {
      tts_audio_callback(NULL, 0);
    }
    return;
  }

  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    int status_code = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);

    if (status_code == 200) {
      ESP_LOGI(TAG, "TTS download complete: status=%d, length=%d", status_code,
               content_length);

      // Send empty chunk to signal end of TTS audio
      if (tts_audio_callback) {
        tts_audio_callback(NULL, 0);
      }
    } else {
      ESP_LOGE(TAG, "HTTP error: status=%d", status_code);
      // Signal error to TTS player
      if (tts_audio_callback) {
        tts_audio_callback(NULL, 0);
      }
    }
  } else {
    ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    // Signal error to TTS player
    if (tts_audio_callback) {
      tts_audio_callback(NULL, 0);
    }
  }

  esp_http_client_cleanup(client);
}

/**
 * Initialize mDNS
 */
static esp_err_t init_mdns(void) {
  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
    return err;
  }

  mdns_hostname_set("esp32-p4-voice-assistant");
  mdns_instance_name_set("ESP32-P4 Voice Assistant");

  ESP_LOGI(TAG, "mDNS initialized");
  return ESP_OK;
}

esp_err_t ha_client_init(void) {
  ESP_LOGI(TAG, "Initializing Home Assistant client...");

  ha_event_group = xEventGroupCreate();

  // Initialize mDNS
  esp_err_t err = init_mdns();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mDNS initialization failed, will use fallback IP");
  }

  // Build WebSocket URI - try using hostname instead of IP
  // Home Assistant may check Host header and reject mismatches
  char ws_uri[256];
  if (HA_USE_SSL) {
    snprintf(ws_uri, sizeof(ws_uri), "wss://%s:%d%s", HA_HOSTNAME, HA_PORT,
             HA_WEBSOCKET_PATH);
  } else {
    snprintf(ws_uri, sizeof(ws_uri), "ws://%s:%d%s", HA_HOSTNAME, HA_PORT,
             HA_WEBSOCKET_PATH);
  }
  ESP_LOGI(TAG, "Connecting to: %s", ws_uri);

  // Configure WebSocket client
  esp_websocket_client_config_t ws_cfg = {
      .uri = ws_uri,
      .task_stack = 8192,
      .buffer_size = 4096,
      .reconnect_timeout_ms = 10000,
      .network_timeout_ms = 30000,
      .pingpong_timeout_sec = 10,
  };

  if (HA_USE_SSL) {
    // Use WSS (WebSocket Secure) with self-signed certificate
    // Python equivalent: sslopt={'cert_reqs': ssl.CERT_NONE, 'check_hostname':
    // False}
    ws_cfg.transport = WEBSOCKET_TRANSPORT_OVER_SSL;

    // Skip ALL certificate verification - relying on sdkconfig:
    // CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y
    ws_cfg.skip_cert_common_name_check = true;
    ws_cfg.use_global_ca_store = false;
    ws_cfg.crt_bundle_attach = NULL;
    // Leave cert_pem as NULL and rely on CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY

    ESP_LOGI(
        TAG,
        "SSL/TLS enabled, relying on CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY");
  } else {
    ESP_LOGI(TAG, "Using plain WebSocket (no SSL)");
  }

  ws_client = esp_websocket_client_init(&ws_cfg);
  if (ws_client == NULL) {
    ESP_LOGE(TAG, "Failed to initialize WebSocket client");
    return ESP_FAIL;
  }

  esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY,
                                websocket_event_handler, NULL);

  err = esp_websocket_client_start(ws_client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WebSocket client start failed: %s", esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(TAG, "WebSocket client started, waiting for authentication...");

  // Wait for authentication (timeout 10 seconds)
  EventBits_t bits =
      xEventGroupWaitBits(ha_event_group, HA_AUTHENTICATED_BIT, pdFALSE,
                          pdFALSE, pdMS_TO_TICKS(10000));

  if (bits & HA_AUTHENTICATED_BIT) {
    ESP_LOGI(TAG, "Home Assistant client initialized successfully");
    return ESP_OK;
  } else {
    ESP_LOGE(TAG, "Authentication timeout");
    return ESP_ERR_TIMEOUT;
  }
}

bool ha_client_is_connected(void) { return ws_connected && ws_authenticated; }

esp_err_t ha_client_send_text(const char *text) {
  if (!ha_client_is_connected()) {
    ESP_LOGW(TAG, "Not connected to Home Assistant");
    return ESP_FAIL;
  }

  // Build conversation/process message
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "id", message_id++);
  cJSON_AddStringToObject(root, "type", "conversation/process");
  cJSON_AddStringToObject(root, "text", text);

  char *json_str = cJSON_PrintUnformatted(root);
  if (json_str == NULL) {
    cJSON_Delete(root);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Sending to HA: %s", json_str);
  int ret = esp_websocket_client_send_text(ws_client, json_str,
                                           strlen(json_str), portMAX_DELAY);

  free(json_str);
  cJSON_Delete(root);

  return (ret >= 0) ? ESP_OK : ESP_FAIL;
}

char *ha_client_start_conversation(void) {
  if (!ha_client_is_connected()) {
    ESP_LOGW(TAG, "Cannot start conversation - not connected");
    return NULL;
  }

  // Start Assist Pipeline with audio input
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "id", message_id++);
  cJSON_AddStringToObject(root, "type", "assist_pipeline/run");

  // Pipeline configuration with STT input
  cJSON_AddStringToObject(root, "start_stage", "stt");
  cJSON_AddStringToObject(root, "end_stage", "tts");

  // Input object with audio format
  cJSON *input = cJSON_CreateObject();
  cJSON_AddNumberToObject(input, "sample_rate", 16000);
  cJSON_AddNumberToObject(input, "num_channels", 1);
  cJSON_AddNumberToObject(input, "bit_depth", 16);
  cJSON_AddItemToObject(root, "input", input);

  char *json_str = cJSON_PrintUnformatted(root);
  if (json_str == NULL) {
    cJSON_Delete(root);
    return NULL;
  }

  ESP_LOGI(TAG, "Starting Assist Pipeline: %s", json_str);
  int ret = esp_websocket_client_send_text(ws_client, json_str,
                                           strlen(json_str), portMAX_DELAY);

  free(json_str);
  cJSON_Delete(root);

  if (ret < 0) {
    ESP_LOGE(TAG, "Failed to start pipeline");
    return NULL;
  }

  // Generate handler ID for this run
  char *handler_id = malloc(37);
  if (handler_id) {
    snprintf(handler_id, 37, "run_%08x", (unsigned int)esp_random());
  }

  return handler_id;
}

esp_err_t ha_client_stream_audio(const uint8_t *audio_data, size_t length,
                                 const char *conversation_id) {
  if (!ha_client_is_connected()) {
    ESP_LOGW(TAG, "Cannot stream audio - not connected");
    return ESP_FAIL;
  }

  if (audio_data == NULL || length == 0) {
    ESP_LOGW(TAG, "Invalid audio data");
    return ESP_ERR_INVALID_ARG;
  }

  if (stt_binary_handler_id < 0) {
    ESP_LOGW(TAG, "STT binary handler ID not set - wait for run-start event");
    return ESP_ERR_INVALID_STATE;
  }

  // HA Assist Pipeline expects binary frames with format:
  // [1 byte: handler_id][N bytes: PCM audio data]
  // Audio format: 16-bit PCM @ 16kHz mono
  uint8_t *frame = malloc(1 + length);
  if (frame == NULL) {
    ESP_LOGE(TAG, "Failed to allocate binary frame");
    return ESP_ERR_NO_MEM;
  }

  frame[0] = (uint8_t)stt_binary_handler_id;
  memcpy(frame + 1, audio_data, length);

  int ret = esp_websocket_client_send_bin(ws_client, (const char *)frame,
                                          1 + length, portMAX_DELAY);
  free(frame);

  if (ret < 0) {
    ESP_LOGE(TAG, "Failed to send audio chunk (%d bytes)", length);
    return ESP_FAIL;
  }

  ESP_LOGD(TAG, "Sent audio chunk: %d bytes (handler_id=%d)", length,
           stt_binary_handler_id);
  return ESP_OK;
}

esp_err_t ha_client_end_audio_stream(void) {
  if (!ha_client_is_connected()) {
    ESP_LOGW(TAG, "Cannot end audio stream - not connected");
    return ESP_FAIL;
  }

  if (stt_binary_handler_id < 0) {
    ESP_LOGW(TAG, "STT binary handler ID not set");
    return ESP_ERR_INVALID_STATE;
  }

  // Send binary frame with just handler ID (no audio data) to signal end
  uint8_t end_frame[1] = {(uint8_t)stt_binary_handler_id};
  int ret = esp_websocket_client_send_bin(ws_client, (const char *)end_frame, 1,
                                          portMAX_DELAY);

  if (ret < 0) {
    ESP_LOGE(TAG, "Failed to send end-of-audio signal");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Audio stream ended (handler_id=%d)", stt_binary_handler_id);

  // Reset handler ID for next conversation
  stt_binary_handler_id = -1;

  return ESP_OK;
}

void ha_client_register_conversation_callback(
    ha_conversation_callback_t callback) {
  conversation_callback = callback;
  ESP_LOGI(TAG, "Conversation callback registered");
}

void ha_client_register_tts_audio_callback(ha_tts_audio_callback_t callback) {
  tts_audio_callback = callback;
  ESP_LOGI(TAG, "TTS audio callback registered");
}

void ha_client_register_error_callback(ha_pipeline_error_callback_t callback) {
  error_callback = callback;
  ESP_LOGI(TAG, "Pipeline error callback registered");
}

void ha_client_register_intent_callback(ha_intent_callback_t callback) {
  intent_callback = callback;
  ESP_LOGI(TAG, "Intent callback registered");
}

esp_err_t ha_client_request_tts(const char *text) {
  if (!ha_client_is_connected()) {
    ESP_LOGW(TAG, "Cannot request TTS - not connected");
    return ESP_FAIL;
  }

  if (text == NULL || strlen(text) == 0) {
    ESP_LOGW(TAG, "Invalid TTS text");
    return ESP_ERR_INVALID_ARG;
  }

  // Build assist_pipeline/run message with text input (bypassing STT)
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "id", message_id++);
  cJSON_AddStringToObject(root, "type", "assist_pipeline/run");

  // Start from intent stage (skip STT) and end at TTS
  cJSON_AddStringToObject(root, "start_stage", "intent");
  cJSON_AddStringToObject(root, "end_stage", "tts");

  // Input object with text
  cJSON *input = cJSON_CreateObject();
  cJSON_AddStringToObject(input, "text", text);
  cJSON_AddItemToObject(root, "input", input);

  char *json_str = cJSON_PrintUnformatted(root);
  if (json_str == NULL) {
    cJSON_Delete(root);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Requesting TTS: %s", json_str);
  int ret = esp_websocket_client_send_text(ws_client, json_str,
                                           strlen(json_str), portMAX_DELAY);

  free(json_str);
  cJSON_Delete(root);

  return (ret >= 0) ? ESP_OK : ESP_FAIL;
}

void ha_client_stop(void) {
  if (ws_client) {
    esp_websocket_client_stop(ws_client);
    esp_websocket_client_destroy(ws_client);
    ws_client = NULL;
  }

  mdns_free();

  ws_connected = false;
  ws_authenticated = false;
  stt_binary_handler_id = -1;

  ESP_LOGI(TAG, "Home Assistant client stopped");
}
