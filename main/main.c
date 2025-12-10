#include "audio_player.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "audio_capture.h"
#include "bsp/esp32_p4_function_ev_board.h"
#include "button_gpio.h"
#include "config.h"
#include "connection_manager.h"
#include "file_iterator.h"
#include "ha_client.h"
#include "iot_button.h"
#include "led_status.h"
#include "local_music_player.h"
#include "mqtt_ha.h"
#include "network_manager.h"
#include "ota_update.h"
#include "tts_player.h"
#include "webserial.h"
#include "wifi_manager.h"
#include "wwd.h"

#define TAG "mp3_player"
#define BUTTON_IO_NUM 35
#define BUTTON_ACTIVE_LEVEL 0

// Forward declarations
static void music_player_event_handler(music_state_t state, int current_track, int total_tracks);

static void conversation_response_handler(const char *response_text,
                                          const char *conversation_id) {
  ESP_LOGI(TAG, "HA Response [%s]: %s",
           conversation_id ? conversation_id : "none", response_text);

  // Set LED to SPEAKING (cyan)
  led_status_set(LED_STATUS_SPEAKING);

  // ═══════════════════════════════════════════════════════════════════
  // >>> NOVO: Publish VA status i response za CYD display <<<
  // ═══════════════════════════════════════════════════════════════════
  if (mqtt_ha_is_connected()) {
    mqtt_ha_update_sensor("va_status", "GOVORIM...");
    if (response_text) {
      mqtt_ha_update_sensor("va_response", response_text);
    }
  }
  // ═══════════════════════════════════════════════════════════════════

  // Parse music control commands from HA response
  if (response_text && local_music_player_is_initialized()) {
    char *response_lower = strdup(response_text);
    if (response_lower) {
      for (char *p = response_lower; *p; p++) {
        *p = tolower((unsigned char)*p);
      }

      if (strstr(response_lower, "play music") ||
          strstr(response_lower, "start music") ||
          strstr(response_lower, "play song")) {
        ESP_LOGI(TAG, "🎵 Voice command: Play music");
        local_music_player_play();
      } else if (strstr(response_lower, "stop music") ||
                 strstr(response_lower, "stop song")) {
        ESP_LOGI(TAG, "🎵 Voice command: Stop music");
        local_music_player_stop();
      } else if (strstr(response_lower, "pause music") ||
                 strstr(response_lower, "pause song")) {
        ESP_LOGI(TAG, "🎵 Voice command: Pause music");
        local_music_player_pause();
      } else if (strstr(response_lower, "resume music") ||
                 strstr(response_lower, "resume song") ||
                 strstr(response_lower, "continue music")) {
        ESP_LOGI(TAG, "🎵 Voice command: Resume music");
        local_music_player_resume();
      } else if (strstr(response_lower, "next song") ||
                 strstr(response_lower, "next track") ||
                 strstr(response_lower, "skip")) {
        ESP_LOGI(TAG, "🎵 Voice command: Next track");
        local_music_player_next();
      } else if (strstr(response_lower, "previous song") ||
                 strstr(response_lower, "previous track") ||
                 strstr(response_lower, "go back")) {
        ESP_LOGI(TAG, "🎵 Voice command: Previous track");
        local_music_player_previous();
      }

      free(response_lower);
    }
  }
}

static void network_event_callback(network_type_t type, bool connected) {
  if (connected) {
    char ip_str[16];
    network_manager_get_ip(ip_str);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Network Connected!");
    ESP_LOGI(TAG, "Type: %s", network_manager_type_to_string(type));
    ESP_LOGI(TAG, "IP Address: %s", ip_str);
    ESP_LOGI(TAG, "========================================");

    if (mqtt_ha_is_connected()) {
      mqtt_ha_update_sensor("network_type",
                            network_manager_type_to_string(type));
      mqtt_ha_update_sensor("ip_address", ip_str);
    }

    if (type == NETWORK_TYPE_ETHERNET) {
      ESP_LOGI(TAG, "📀 Ethernet active - mounting SD card for local music...");
      esp_err_t sd_ret = bsp_sdcard_mount();
      if (sd_ret == ESP_OK) {
        ESP_LOGI(
            TAG,
            "✅ SD card mounted successfully - local music playback enabled");

        esp_err_t music_ret = local_music_player_init();
        if (music_ret == ESP_OK) {
          ESP_LOGI(TAG, "🎵 Local music player initialized - %d tracks found",
                   local_music_player_get_total_tracks());

          // Register music player event callback
          local_music_player_register_callback(music_player_event_handler);
          ESP_LOGI(TAG, "Music player event callback registered");

          if (mqtt_ha_is_connected()) {
            mqtt_ha_update_sensor("sd_card_status", "ready");
          }
        } else {
          ESP_LOGW(TAG, "⚠️  Music player init failed - no music files found?");
          if (mqtt_ha_is_connected()) {
            mqtt_ha_update_sensor("sd_card_status", "no_music");
          }
        }
      } else {
        ESP_LOGW(TAG, "⚠️  SD card mount failed - local music unavailable");
        if (mqtt_ha_is_connected()) {
          mqtt_ha_update_sensor("sd_card_status", "failed");
        }
      }
    } else if (type == NETWORK_TYPE_WIFI) {
      ESP_LOGI(TAG, "📶 WiFi fallback active - SD card disabled");

      if (local_music_player_is_initialized()) {
        ESP_LOGI(TAG, "Stopping local music player...");
        local_music_player_deinit();
      }

      if (bsp_sdcard != NULL) {
        ESP_LOGI(TAG, "Unmounting SD card (WiFi fallback mode)...");
        bsp_sdcard_unmount();
        if (mqtt_ha_is_connected()) {
          mqtt_ha_update_sensor("sd_card_status", "unmounted");
        }
      }
    }
  } else {
    ESP_LOGW(TAG, "Network disconnected: %s",
             network_manager_type_to_string(type));

    if (local_music_player_is_initialized()) {
      ESP_LOGI(TAG, "Stopping local music player...");
      local_music_player_deinit();
    }

    if (bsp_sdcard != NULL) {
      ESP_LOGI(TAG, "Unmounting SD card (network disconnected)...");
      bsp_sdcard_unmount();
      if (mqtt_ha_is_connected()) {
        mqtt_ha_update_sensor("sd_card_status", "disconnected");
      }
    }
  }
}

static bool music_paused_for_tts = false;

static void tts_audio_handler(const uint8_t *audio_data, size_t length) {
  ESP_LOGI(TAG, "Received TTS audio: %d bytes", length);

  if (local_music_player_is_initialized() &&
      local_music_player_get_state() == MUSIC_STATE_PLAYING) {
    ESP_LOGI(TAG, "Pausing music for TTS playback");
    local_music_player_pause();
    music_paused_for_tts = true;
  }

  esp_err_t ret = tts_player_feed(audio_data, length);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to feed TTS audio: %s", esp_err_to_name(ret));
  }
}

static char *pipeline_handler = NULL;
static int audio_chunks_sent = 0;
static bool pipeline_active = false;
static int warmup_chunks_skip = 0;

static uint32_t vad_threshold = 180;
static uint32_t vad_silence_duration = 1800;
static uint32_t vad_min_speech = 200;
static uint32_t vad_max_recording = 7000;
static float wwd_threshold = 0.5f;

// AGC configuration
static bool agc_enabled = true; // Enabled by default
static uint16_t agc_target_level = 4000;

static void test_audio_streaming(void);
static void wwd_audio_feed_wrapper(const int16_t *audio_data, size_t samples);
static void on_wake_word_detected(wwd_event_t event, void *user_data);

static void mqtt_wwd_switch_callback(const char *entity_id,
                                     const char *payload) {
  ESP_LOGI(TAG, "MQTT: WWD switch = %s", payload);

  if (strcmp(payload, "ON") == 0) {
    wwd_start();
    audio_capture_start_wake_word_mode(wwd_audio_feed_wrapper);
    mqtt_ha_update_switch("wwd_enabled", true);
    led_status_set(LED_STATUS_IDLE); // Green - wake word ready
    ESP_LOGI(TAG, "Wake Word Detection enabled via MQTT");
  } else {
    wwd_stop();
    audio_capture_stop();
    mqtt_ha_update_switch("wwd_enabled", false);
    ESP_LOGI(TAG, "Wake Word Detection disabled via MQTT");
  }
}

static void mqtt_webserial_switch_callback(const char *entity_id,
                                           const char *payload) {
  ESP_LOGI(TAG, "MQTT: WebSerial switch = %s", payload);

  if (strcmp(payload, "ON") == 0) {
    if (!webserial_is_running()) {
      esp_err_t ret = webserial_init();
      if (ret == ESP_OK) {
        mqtt_ha_update_switch("webserial_enabled", true);
        ESP_LOGI(TAG, "WebSerial enabled via MQTT");
      } else {
        ESP_LOGE(TAG, "Failed to start WebSerial");
        mqtt_ha_update_switch("webserial_enabled", false);
      }
    }
  } else {
    if (webserial_is_running()) {
      webserial_deinit();
      mqtt_ha_update_switch("webserial_enabled", false);
      ESP_LOGI(TAG, "WebSerial disabled via MQTT");
    }
  }
}

static void mqtt_music_play_callback(const char *entity_id,
                                     const char *payload) {
  ESP_LOGI(TAG, "MQTT: Play music button pressed");
  if (local_music_player_is_initialized()) {
    local_music_player_play();
  } else {
    ESP_LOGW(TAG, "Music player not initialized (SD card not mounted?)");
  }
}

static void mqtt_music_stop_callback(const char *entity_id,
                                     const char *payload) {
  ESP_LOGI(TAG, "MQTT: Stop music button pressed");
  if (local_music_player_is_initialized()) {
    local_music_player_stop();
  }
}

static void mqtt_music_pause_callback(const char *entity_id,
                                      const char *payload) {
  ESP_LOGI(TAG, "MQTT: Pause music button pressed");
  if (local_music_player_is_initialized()) {
    local_music_player_pause();
  }
}

static void mqtt_music_resume_callback(const char *entity_id,
                                       const char *payload) {
  ESP_LOGI(TAG, "MQTT: Resume music button pressed");
  if (local_music_player_is_initialized()) {
    local_music_player_resume();
  }
}

static void mqtt_music_next_callback(const char *entity_id,
                                     const char *payload) {
  ESP_LOGI(TAG, "MQTT: Next track button pressed");
  if (local_music_player_is_initialized()) {
    local_music_player_next();
  }
}

static void mqtt_music_previous_callback(const char *entity_id,
                                         const char *payload) {
  ESP_LOGI(TAG, "MQTT: Previous track button pressed");
  if (local_music_player_is_initialized()) {
    local_music_player_previous();
  }
}

static void mqtt_restart_callback(const char *entity_id, const char *payload) {
  ESP_LOGI(TAG, "MQTT: Restart button pressed!");
  ESP_LOGI(TAG, "Restarting in 2 seconds...");
  vTaskDelay(pdMS_TO_TICKS(2000));
  esp_restart();
}

static void mqtt_test_tts_callback(const char *entity_id, const char *payload) {
  ESP_LOGI(TAG, "MQTT: Test TTS button pressed!");

  if (ha_client_is_connected()) {
    ESP_LOGI(TAG, "Requesting test TTS audio...");
    esp_err_t ret = ha_client_request_tts("Koliko je sati?");
    if (ret == ESP_OK) {
      ESP_LOGI(TAG, "Test TTS request sent successfully");
    } else {
      ESP_LOGE(TAG, "Failed to send test TTS request");
    }
  } else {
    ESP_LOGW(TAG, "Cannot test TTS - not connected to Home Assistant");
  }
}

static char ota_url_buffer[256] = "";

// Load OTA URL from NVS on startup
static void load_ota_url_from_nvs(void) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open("ota", NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    size_t len = sizeof(ota_url_buffer);
    err = nvs_get_str(nvs, "url", ota_url_buffer, &len);
    if (err == ESP_OK && strlen(ota_url_buffer) > 0) {
      ESP_LOGI(TAG, "Loaded OTA URL from NVS: %s", ota_url_buffer);
    }
    nvs_close(nvs);
  }
}

// Save OTA URL to NVS
static void save_ota_url_to_nvs(const char *url) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open("ota", NVS_READWRITE, &nvs);
  if (err == ESP_OK) {
    nvs_set_str(nvs, "url", url);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "OTA URL saved to NVS");
  }
}

static void mqtt_ota_url_callback(const char *entity_id, const char *payload) {
  ESP_LOGI(TAG, "MQTT: OTA URL received: %s", payload);

  // Store URL in buffer
  strncpy(ota_url_buffer, payload, sizeof(ota_url_buffer) - 1);
  ota_url_buffer[sizeof(ota_url_buffer) - 1] = '\0';

  // Save to NVS for persistence
  save_ota_url_to_nvs(ota_url_buffer);

  // Update sensor to confirm receipt
  mqtt_ha_update_text("ota_url_input", ota_url_buffer);
  mqtt_ha_update_sensor("ota_url", ota_url_buffer);

  ESP_LOGI(TAG, "OTA URL stored: %s", ota_url_buffer);
}

static void mqtt_ota_trigger_callback(const char *entity_id,
                                      const char *payload) {
  ESP_LOGI(TAG, "MQTT: OTA update button pressed");

  if (strlen(ota_url_buffer) == 0) {
    ESP_LOGE(TAG, "OTA update failed: No URL configured");
    mqtt_ha_update_sensor("ota_status", "error: no URL");
    return;
  }

  if (ota_update_is_running()) {
    ESP_LOGW(TAG, "OTA update already in progress");
    mqtt_ha_update_sensor("ota_status", "already running");
    return;
  }

  ESP_LOGI(TAG, "Starting OTA update from: %s", ota_url_buffer);

  if (local_music_player_is_initialized() &&
      local_music_player_get_state() == MUSIC_STATE_PLAYING) {
    ESP_LOGI(TAG, "Stopping music for OTA update");
    local_music_player_stop();
  }

  mqtt_ha_update_sensor("ota_status", "starting");

  esp_err_t ret = ota_update_start(ota_url_buffer);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start OTA update");
    mqtt_ha_update_sensor("ota_status", "failed to start");
  }
}

static void ota_progress_callback(ota_state_t state, int progress,
                                  const char *message) {
  ESP_LOGI(TAG, "OTA [%d%%]: %s", progress, message);

  char progress_str[8];
  snprintf(progress_str, sizeof(progress_str), "%d", progress);
  mqtt_ha_update_sensor("ota_progress", progress_str);

  switch (state) {
  case OTA_STATE_IDLE:
    mqtt_ha_update_sensor("ota_status", "idle");
    break;
  case OTA_STATE_DOWNLOADING:
    mqtt_ha_update_sensor("ota_status", "downloading");
    break;
  case OTA_STATE_VERIFYING:
    mqtt_ha_update_sensor("ota_status", "verifying");
    break;
  case OTA_STATE_SUCCESS:
    mqtt_ha_update_sensor("ota_status", "success");
    break;
  case OTA_STATE_FAILED:
    mqtt_ha_update_sensor("ota_status", "failed");
    break;
  }
}

static void mqtt_vad_threshold_callback(const char *entity_id,
                                        const char *payload) {
  float value = atof(payload);
  vad_threshold = (uint32_t)value;
  ESP_LOGI(TAG, "MQTT: VAD threshold updated to %lu", vad_threshold);
  mqtt_ha_update_number("vad_threshold", (float)vad_threshold);
}

static void mqtt_vad_silence_callback(const char *entity_id,
                                      const char *payload) {
  float value = atof(payload);
  vad_silence_duration = (uint32_t)value;
  ESP_LOGI(TAG, "MQTT: VAD silence duration updated to %lums",
           vad_silence_duration);
  mqtt_ha_update_number("vad_silence_duration", (float)vad_silence_duration);
}

static void mqtt_vad_min_speech_callback(const char *entity_id,
                                         const char *payload) {
  float value = atof(payload);
  vad_min_speech = (uint32_t)value;
  ESP_LOGI(TAG, "MQTT: VAD min speech updated to %lums", vad_min_speech);
  mqtt_ha_update_number("vad_min_speech", (float)vad_min_speech);
}

static void mqtt_vad_max_recording_callback(const char *entity_id,
                                            const char *payload) {
  float value = atof(payload);
  vad_max_recording = (uint32_t)value;
  ESP_LOGI(TAG, "MQTT: VAD max recording updated to %lums", vad_max_recording);
  mqtt_ha_update_number("vad_max_recording", (float)vad_max_recording);
}

static void mqtt_wwd_threshold_callback(const char *entity_id,
                                        const char *payload) {
  wwd_threshold = atof(payload);
  ESP_LOGI(TAG, "MQTT: WWD threshold updated to %.2f", wwd_threshold);

  if (wwd_is_running()) {
    wwd_stop();
    audio_capture_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    wwd_config_t wwd_config = wwd_get_default_config();
    wwd_config.callback = on_wake_word_detected;
    wwd_config.user_data = NULL;
    wwd_config.detection_threshold = wwd_threshold;

    esp_err_t ret = wwd_init(&wwd_config);
    if (ret == ESP_OK) {
      wwd_start();
      audio_capture_start_wake_word_mode(wwd_audio_feed_wrapper);
      ESP_LOGI(TAG, "WWD restarted with new threshold");
    } else {
      ESP_LOGE(TAG, "Failed to restart WWD with new threshold");
    }
  }

  mqtt_ha_update_number("wwd_threshold", wwd_threshold);
}

// ═══════════════════════════════════════════════════════════════════════════
// AGC MQTT Callbacks
// ═══════════════════════════════════════════════════════════════════════════

static void mqtt_agc_switch_callback(const char *entity_id,
                                     const char *payload) {
  ESP_LOGI(TAG, "MQTT: AGC switch = %s", payload);

  if (strcmp(payload, "ON") == 0) {
    esp_err_t ret = audio_capture_enable_agc(agc_target_level);
    if (ret == ESP_OK) {
      agc_enabled = true;
      mqtt_ha_update_switch("agc_enabled", true);
      ESP_LOGI(TAG, "AGC enabled via MQTT");
    }
  } else {
    audio_capture_disable_agc();
    agc_enabled = false;
    mqtt_ha_update_switch("agc_enabled", false);
    ESP_LOGI(TAG, "AGC disabled via MQTT");
  }
}

static void mqtt_agc_target_callback(const char *entity_id,
                                     const char *payload) {
  float value = atof(payload);
  agc_target_level = (uint16_t)value;
  ESP_LOGI(TAG, "MQTT: AGC target level updated to %u", agc_target_level);

  if (agc_enabled) {
    audio_capture_set_agc_target(agc_target_level);
  }

  mqtt_ha_update_number("agc_target_level", (float)agc_target_level);
}

// ═══════════════════════════════════════════════════════════════════════════
// LED Status MQTT Callbacks
// ═══════════════════════════════════════════════════════════════════════════

static void mqtt_led_switch_callback(const char *entity_id,
                                     const char *payload) {
  bool enable = (strcmp(payload, "ON") == 0);
  ESP_LOGI(TAG, "MQTT: LED %s", enable ? "enabled" : "disabled");
  led_status_enable(enable);
  mqtt_ha_update_switch("led_enabled", enable);
}

static void mqtt_led_brightness_callback(const char *entity_id,
                                         const char *payload) {
  float value = atof(payload);
  uint8_t brightness = (uint8_t)value;
  ESP_LOGI(TAG, "MQTT: LED brightness updated to %u%%", brightness);
  led_status_set_brightness(brightness);
  mqtt_ha_update_number("led_brightness", (float)brightness);
}

static void mqtt_status_update_task(void *arg) {
  while (1) {
    if (mqtt_ha_is_connected()) {
      mqtt_ha_update_sensor("wifi_rssi", "-45");

      char mem_str[32];
      uint32_t free_mem = esp_get_free_heap_size() / 1024;
      snprintf(mem_str, sizeof(mem_str), "%lu", (unsigned long)free_mem);
      mqtt_ha_update_sensor("free_memory", mem_str);

      char uptime_str[32];
      uint32_t uptime_sec = esp_log_timestamp() / 1000;
      snprintf(uptime_str, sizeof(uptime_str), "%lu",
               (unsigned long)uptime_sec);
      mqtt_ha_update_sensor("uptime", uptime_str);

      mqtt_ha_update_switch("wwd_enabled", wwd_is_running());

      mqtt_ha_update_switch("webserial_enabled", webserial_is_running());
      char webserial_clients_str[8];
      snprintf(webserial_clients_str, sizeof(webserial_clients_str), "%d",
               webserial_get_client_count());
      mqtt_ha_update_sensor("webserial_clients", webserial_clients_str);

      char ip_str[16];
      if (network_manager_get_ip(ip_str) == ESP_OK) {
        mqtt_ha_update_sensor("ip_address", ip_str);
      }
      mqtt_ha_update_sensor(
          "network_type",
          network_manager_type_to_string(network_manager_get_active_type()));

      // Report AGC current gain
      if (audio_capture_is_agc_enabled()) {
        char agc_gain_str[16];
        snprintf(agc_gain_str, sizeof(agc_gain_str), "%.2f",
                 audio_capture_get_agc_gain());
        mqtt_ha_update_sensor("agc_current_gain", agc_gain_str);
      }

      if (bsp_sdcard != NULL) {
        mqtt_ha_update_sensor("sd_card_status", "mounted");
      } else {
        mqtt_ha_update_sensor("sd_card_status", "not_mounted");
      }

      if (local_music_player_is_initialized()) {
        music_state_t state = local_music_player_get_state();
        const char *state_str;
        switch (state) {
        case MUSIC_STATE_PLAYING:
          state_str = "playing";
          break;
        case MUSIC_STATE_PAUSED:
          state_str = "paused";
          break;
        case MUSIC_STATE_STOPPED:
          state_str = "stopped";
          break;
        default:
          state_str = "idle";
          break;
        }
        mqtt_ha_update_sensor("music_state", state_str);

        char track_name[64];
        if (local_music_player_get_track_name(track_name, sizeof(track_name)) ==
            ESP_OK) {
          mqtt_ha_update_sensor("current_track", track_name);
        } else {
          mqtt_ha_update_sensor("current_track", "No track");
        }

        char total_tracks_str[16];
        snprintf(total_tracks_str, sizeof(total_tracks_str), "%d",
                 local_music_player_get_total_tracks());
        mqtt_ha_update_sensor("total_tracks", total_tracks_str);
      } else {
        mqtt_ha_update_sensor("music_state", "unavailable");
        mqtt_ha_update_sensor("current_track", "N/A");
        mqtt_ha_update_sensor("total_tracks", "0");
      }

      mqtt_ha_update_sensor("firmware_version",
                            ota_update_get_current_version());
      if (strlen(ota_url_buffer) > 0) {
        mqtt_ha_update_sensor("ota_url", ota_url_buffer);
      } else {
        mqtt_ha_update_sensor("ota_url", "Not configured");
      }
      if (!ota_update_is_running()) {
        mqtt_ha_update_sensor("ota_status", "idle");
        mqtt_ha_update_sensor("ota_progress", "0");
      }

      static bool config_published = false;
      if (!config_published) {
        mqtt_ha_update_number("vad_threshold", (float)vad_threshold);
        mqtt_ha_update_number("vad_silence_duration",
                              (float)vad_silence_duration);
        mqtt_ha_update_number("vad_min_speech", (float)vad_min_speech);
        mqtt_ha_update_number("vad_max_recording", (float)vad_max_recording);
        mqtt_ha_update_number("wwd_threshold", wwd_threshold);
        config_published = true;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// WAKE WORD DETECTION CALLBACK
// ═══════════════════════════════════════════════════════════════════════════
static void on_wake_word_detected(wwd_event_t event, void *user_data) {
  if (event == WWD_EVENT_DETECTED) {
    ESP_LOGI(TAG, "🎤 Wake word detected!");

    // Set LED to LISTENING (blue pulsing)
    led_status_set(LED_STATUS_LISTENING);

    // ═══════════════════════════════════════════════════════════════════
    // >>> NOVO: Publish VA status za CYD display <<<
    // ═══════════════════════════════════════════════════════════════════
    if (mqtt_ha_is_connected()) {
      mqtt_ha_update_sensor("va_status", "SLUŠAM...");
    }
    // ═══════════════════════════════════════════════════════════════════

    wwd_stop();
    audio_capture_stop();
    ESP_LOGI(TAG, "Wake word mode stopped");

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "🔊 Playing wake word confirmation beep...");
    extern esp_err_t beep_tone_play(uint16_t frequency, uint16_t duration,
                                    uint8_t volume);
    esp_err_t beep_ret = beep_tone_play(800, 120, 40);
    if (beep_ret != ESP_OK) {
      ESP_LOGW(TAG, "Failed to play beep tone: %s", esp_err_to_name(beep_ret));
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "Starting voice pipeline...");
    test_audio_streaming();
  }
}

static void wwd_audio_feed_wrapper(const int16_t *audio_data, size_t samples) {
  wwd_feed_audio(audio_data, samples);
}

// ═══════════════════════════════════════════════════════════════════════════
// TTS PLAYBACK COMPLETE HANDLER
// ═══════════════════════════════════════════════════════════════════════════
static void tts_playback_complete_handler(void) {
  ESP_LOGI(TAG, "🔄 TTS playback complete - resuming wake word detection...");

  // ═══════════════════════════════════════════════════════════════════
  // >>> NOVO: Publish VA status za CYD display <<<
  // ═══════════════════════════════════════════════════════════════════
  if (mqtt_ha_is_connected()) {
    mqtt_ha_update_sensor("va_status", "SPREMAN");
  }
  // ═══════════════════════════════════════════════════════════════════

  // Set LED back to IDLE (dim green)
  led_status_set(LED_STATUS_IDLE);

  if (music_paused_for_tts && local_music_player_is_initialized()) {
    ESP_LOGI(TAG, "Resuming music playback after TTS");
    local_music_player_resume();
    music_paused_for_tts = false;
  }

  wwd_start();
  audio_capture_start_wake_word_mode(wwd_audio_feed_wrapper);

  ESP_LOGI(TAG, "✅ Wake word detection resumed - ready for next command");
}

// ═══════════════════════════════════════════════════════════════════════════
// PIPELINE ERROR HANDLER
// ═══════════════════════════════════════════════════════════════════════════
static void pipeline_error_handler(const char *error_code, const char *error_message) {
  ESP_LOGE(TAG, "Pipeline error occurred: %s - %s", error_code, error_message);

  // Set LED to ERROR (red blinking)
  led_status_set(LED_STATUS_ERROR);

  // Clean up pipeline state
  pipeline_active = false;
  if (pipeline_handler != NULL) {
    free(pipeline_handler);
    pipeline_handler = NULL;
  }
  audio_chunks_sent = 0;

  ESP_LOGI(TAG, "Pipeline state cleaned up, resuming wake word detection in 2 seconds...");

  // Wait a bit before resuming wake word mode
  vTaskDelay(pdMS_TO_TICKS(2000));

  // Resume wake word detection
  wwd_start();
  audio_capture_start_wake_word_mode(wwd_audio_feed_wrapper);

  // Set LED to IDLE (green)
  led_status_set(LED_STATUS_IDLE);

  ESP_LOGI(TAG, "✅ Wake word detection resumed after pipeline error");
}

// ═══════════════════════════════════════════════════════════════════════════
// MUSIC PLAYER EVENT HANDLER
// ═══════════════════════════════════════════════════════════════════════════
static void music_player_event_handler(music_state_t state, int current_track, int total_tracks) {
  ESP_LOGI(TAG, "Music player state changed: %d (track %d/%d)", state, current_track + 1, total_tracks);

  if (state == MUSIC_STATE_STOPPED) {
    ESP_LOGI(TAG, "Music playback stopped - resuming wake word detection...");

    // Set LED back to IDLE (green)
    led_status_set(LED_STATUS_IDLE);

    // Resume wake word detection
    wwd_start();
    audio_capture_start_wake_word_mode(wwd_audio_feed_wrapper);

    ESP_LOGI(TAG, "✅ Wake word detection resumed after music stopped");
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// VAD EVENT HANDLER
// ═══════════════════════════════════════════════════════════════════════════
static void vad_event_handler(audio_capture_vad_event_t event) {
  switch (event) {
  case VAD_EVENT_SPEECH_START:
    ESP_LOGI(TAG, "🎤 Speech detected - recording...");
    break;

  case VAD_EVENT_SPEECH_END:
    ESP_LOGI(TAG, "🔇 Silence detected - VAD auto-stop triggered");
    ESP_LOGI(TAG, "Total audio chunks sent: %d", audio_chunks_sent);

    // ═══════════════════════════════════════════════════════════════════
    // >>> NOVO: Publish VA status za CYD display <<<
    // ═══════════════════════════════════════════════════════════════════
    if (mqtt_ha_is_connected()) {
      mqtt_ha_update_sensor("va_status", "OBRAĐUJEM...");
    }
    // ═══════════════════════════════════════════════════════════════════

    // Set LED to PROCESSING (yellow blinking)
    led_status_set(LED_STATUS_PROCESSING);

    pipeline_active = false;
    audio_capture_stop();
    ESP_LOGI(TAG, "Audio capture stopped - I2S freed for TTS");

    ha_client_end_audio_stream();

    if (pipeline_handler != NULL) {
      free(pipeline_handler);
      pipeline_handler = NULL;
    }
    audio_chunks_sent = 0;

    ESP_LOGI(TAG,
             "Waiting for TTS playback to complete before resuming WWD...");
    break;
  }
}

static void audio_capture_handler(const uint8_t *audio_data, size_t length) {
  if (!pipeline_active || pipeline_handler == NULL) {
    return;
  }

  if (audio_data == NULL || length == 0) {
    return;
  }

  if (warmup_chunks_skip > 0) {
    warmup_chunks_skip--;
    ESP_LOGD(TAG, "Skipping warmup chunk (%d remaining)", warmup_chunks_skip);
    return;
  }

  esp_err_t ret = ha_client_stream_audio(audio_data, length, pipeline_handler);
  if (ret == ESP_OK) {
    audio_chunks_sent++;
  } else {
    ESP_LOGW(TAG, "Failed to stream audio chunk - pipeline may be closed");
  }
}

static void test_audio_streaming(void) {
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "Starting Audio Streaming Test with VAD");
  ESP_LOGI(TAG, "========================================");

  vad_config_t vad_config = {.sample_rate = 16000,
                             .speech_threshold = vad_threshold,
                             .silence_duration_ms = vad_silence_duration,
                             .min_speech_duration_ms = vad_min_speech,
                             .max_recording_ms = vad_max_recording};

  ESP_LOGI(TAG,
           "📊 VAD Config: threshold=%lu, silence=%lums, min_speech=%lums, "
           "max=%lums",
           vad_config.speech_threshold, vad_config.silence_duration_ms,
           vad_config.min_speech_duration_ms, vad_config.max_recording_ms);

  esp_err_t ret = audio_capture_enable_vad(&vad_config, vad_event_handler);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to enable VAD");
    return;
  }

  pipeline_handler = ha_client_start_conversation();
  if (pipeline_handler == NULL) {
    ESP_LOGE(TAG, "Failed to start pipeline");
    return;
  }

  ESP_LOGI(TAG, "Pipeline started: %s", pipeline_handler);
  ESP_LOGI(TAG, "🎙️  Start speaking now! (VAD will auto-stop after silence)");

  audio_chunks_sent = 0;
  pipeline_active = true;
  warmup_chunks_skip = 10;

  audio_capture_reset_vad();

  ret = audio_capture_start(audio_capture_handler);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start audio capture");
    free(pipeline_handler);
    pipeline_handler = NULL;
  }
}

void app_main(void) {
  ESP_LOGI(TAG, "===== ESP32-P4 Voice Assistant Starting =====");
  ESP_LOGI(TAG, "===== Firmware Version: 2.1.0 (CYD Integration) =====");

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  ESP_LOGI(TAG, "NVS initialized");

  ESP_LOGI(TAG, "Initializing OTA update module...");
  ret = ota_update_init();
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "OTA module initialized - Version: %s",
             ota_update_get_current_version());
    ota_update_register_callback(ota_progress_callback);

    // Load previously saved OTA URL from NVS
    load_ota_url_from_nvs();
  } else {
    ESP_LOGW(TAG, "OTA module initialization failed");
  }

  // ═══════════════════════════════════════════════════════════════════════
  // Initialize LED Status Indicator
  // ═══════════════════════════════════════════════════════════════════════
  ESP_LOGI(TAG, "Initializing LED status indicator...");
  ret = led_status_init();
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "LED status initialized (R=%d, G=%d, B=%d)", LED_GPIO_RED,
             LED_GPIO_GREEN, LED_GPIO_BLUE);
    // Set LED to yellow - system booting
    led_status_set(LED_STATUS_BOOTING);
  } else {
    ESP_LOGW(TAG, "LED status initialization failed");
  }

  ESP_LOGI(TAG, "Initializing ES8311 audio codec...");
  ESP_ERROR_CHECK(bsp_extra_codec_init());
  bsp_extra_codec_volume_set(40, NULL);
  bsp_extra_player_init();
  ESP_LOGI(TAG, "ES8311 codec initialized successfully");

  ESP_LOGI(TAG, "Initializing TTS player...");
  ret = tts_player_init();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "TTS player initialization failed");
  } else {
    ESP_LOGI(TAG, "TTS player initialized successfully");
  }

  ESP_LOGI(TAG, "Initializing audio capture...");
  ret = audio_capture_init();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Audio capture initialization failed");
  } else {
    ESP_LOGI(TAG, "Audio capture initialized successfully");
  }

  ESP_LOGI(TAG,
           "WakeNet models will be loaded from flash (managed_components)");

  ESP_LOGI(TAG, "Initializing Wake Word Detection...");
  wwd_config_t wwd_config = wwd_get_default_config();
  wwd_config.callback = on_wake_word_detected;
  wwd_config.user_data = NULL;
  wwd_config.detection_threshold = wwd_threshold;
  ESP_LOGI(TAG, "WWD threshold: %.2f, VAD threshold: %lu", wwd_threshold,
           vad_threshold);
  esp_err_t wwd_ret = wwd_init(&wwd_config);

  if (wwd_ret != ESP_OK) {
    ESP_LOGE(TAG, "Wake Word Detection initialization failed!");
    ESP_LOGW(TAG, "Falling back to VAD-based activation");
  } else {
    ESP_LOGI(TAG, "Wake Word Detection initialized successfully!");
  }

  ESP_LOGI(TAG, "Initializing Network Manager...");
  // Set LED to purple pulsing - connecting to network
  led_status_set(LED_STATUS_CONNECTING);
  network_manager_register_callback(network_event_callback);
  ret = network_manager_init();
  if (ret == ESP_OK && network_manager_is_connected()) {
    ESP_LOGI(TAG, "Network connected successfully!");
    ESP_LOGI(TAG, "Active network: %s",
             network_manager_type_to_string(network_manager_get_active_type()));

    // ═══════════════════════════════════════════════════════════════════════
    // Initialize Connection Manager for auto-reconnection
    // ═══════════════════════════════════════════════════════════════════════
    connection_manager_config_t conn_config =
        connection_manager_get_default_config();
    ret = connection_manager_init(&conn_config);
    if (ret == ESP_OK) {
      ESP_LOGI(TAG, "Connection Manager initialized");

      // Register HA WebSocket for monitoring (reconnect via ha_client_init)
      connection_manager_register(CONN_TYPE_HA_WEBSOCKET, "HA WebSocket",
                                  ha_client_init);

      // Register MQTT for monitoring (reconnect via mqtt_ha_start)
      connection_manager_register(CONN_TYPE_MQTT, "MQTT", mqtt_ha_start);

      // Start connection manager monitoring
      connection_manager_start();
      ESP_LOGI(TAG, "Connection Manager started - auto-reconnection enabled");
    } else {
      ESP_LOGW(TAG,
               "Connection Manager init failed - auto-reconnection disabled");
    }
    // ═══════════════════════════════════════════════════════════════════════

    ESP_LOGI(TAG, "Connecting to Home Assistant...");
    ret = ha_client_init();
    if (ret == ESP_OK) {
      ESP_LOGI(TAG, "Home Assistant connected successfully!");

      ha_client_register_conversation_callback(conversation_response_handler);
      ha_client_register_tts_audio_callback(tts_audio_handler);
      ha_client_register_error_callback(pipeline_error_handler);
      tts_player_register_complete_callback(tts_playback_complete_handler);

      ESP_LOGI(TAG, "Initializing MQTT Home Assistant Discovery...");
      mqtt_ha_config_t mqtt_config = {.broker_uri = MQTT_BROKER_URI,
                                      .username = MQTT_USERNAME,
                                      .password = MQTT_PASSWORD,
                                      .client_id = MQTT_CLIENT_ID};

      ret = mqtt_ha_init(&mqtt_config);
      if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MQTT client initialized");

        ret = mqtt_ha_start();
        if (ret == ESP_OK) {
          ESP_LOGI(TAG, "MQTT client started");

          vTaskDelay(pdMS_TO_TICKS(2000));

          ESP_LOGI(TAG, "Registering Home Assistant entities...");

          // Sensors
          mqtt_ha_register_sensor("network_type", "Network Type", NULL, NULL);
          mqtt_ha_register_sensor("ip_address", "IP Address", NULL, NULL);
          mqtt_ha_register_sensor("sd_card_status", "SD Card Status", NULL,
                                  NULL);
          mqtt_ha_register_sensor("wifi_rssi", "WiFi Signal", "dBm",
                                  "signal_strength");
          mqtt_ha_register_sensor("free_memory", "Free Memory", "KB", NULL);
          mqtt_ha_register_sensor("uptime", "Uptime", "s", "duration");

          // ═══════════════════════════════════════════════════════════════════
          // >>> NOVO: VA Status senzori za CYD display integraciju <<<
          // ═══════════════════════════════════════════════════════════════════
          mqtt_ha_register_sensor("va_status", "VA Status", NULL, NULL);
          mqtt_ha_register_sensor("va_response", "VA Response", NULL, NULL);
          // ═══════════════════════════════════════════════════════════════════

          // Switches
          mqtt_ha_register_switch("wwd_enabled", "Wake Word Detection",
                                  mqtt_wwd_switch_callback);
          mqtt_ha_register_switch("webserial_enabled", "WebSerial Console",
                                  mqtt_webserial_switch_callback);

          // Sensors - WebSerial
          mqtt_ha_register_sensor("webserial_clients", "WebSerial Clients",
                                  NULL, NULL);

          // Buttons - System
          mqtt_ha_register_button("restart", "Restart Device",
                                  mqtt_restart_callback);
          mqtt_ha_register_button("test_tts", "Test TTS",
                                  mqtt_test_tts_callback);

          // Buttons - Music Player
          mqtt_ha_register_button("music_play", "Play Music",
                                  mqtt_music_play_callback);
          mqtt_ha_register_button("music_stop", "Stop Music",
                                  mqtt_music_stop_callback);
          mqtt_ha_register_button("music_pause", "Pause Music",
                                  mqtt_music_pause_callback);
          mqtt_ha_register_button("music_resume", "Resume Music",
                                  mqtt_music_resume_callback);
          mqtt_ha_register_button("music_next", "Next Track",
                                  mqtt_music_next_callback);
          mqtt_ha_register_button("music_previous", "Previous Track",
                                  mqtt_music_previous_callback);

          // Sensors - Music Player
          mqtt_ha_register_sensor("music_state", "Music State", NULL, NULL);
          mqtt_ha_register_sensor("current_track", "Current Track", NULL, NULL);
          mqtt_ha_register_sensor("total_tracks", "Total Tracks", NULL, NULL);

          // Sensors - OTA Update
          mqtt_ha_register_sensor("firmware_version", "Firmware Version", NULL,
                                  NULL);
          mqtt_ha_register_sensor("ota_status", "OTA Status", NULL, NULL);
          mqtt_ha_register_sensor("ota_progress", "OTA Progress", "%", NULL);
          mqtt_ha_register_sensor("ota_url", "OTA Update URL", NULL, NULL);

          // Text input for OTA URL
          mqtt_ha_register_text("ota_url_input", "OTA URL Input",
                                mqtt_ota_url_callback);

          // Button to trigger OTA update
          mqtt_ha_register_button("ota_trigger", "Trigger OTA Update",
                                  mqtt_ota_trigger_callback);

          // Number controls for VAD tuning
          mqtt_ha_register_number("vad_threshold", "VAD Speech Threshold", 50,
                                  300, 10, NULL, mqtt_vad_threshold_callback);
          mqtt_ha_register_number("vad_silence_duration",
                                  "VAD Silence Duration", 1000, 3000, 100, "ms",
                                  mqtt_vad_silence_callback);
          mqtt_ha_register_number("vad_min_speech", "VAD Min Speech Duration",
                                  100, 500, 50, "ms",
                                  mqtt_vad_min_speech_callback);
          mqtt_ha_register_number("vad_max_recording",
                                  "VAD Max Recording Duration", 5000, 10000,
                                  500, "ms", mqtt_vad_max_recording_callback);

          // Number control for WWD tuning
          mqtt_ha_register_number("wwd_threshold", "WWD Detection Threshold",
                                  0.3, 0.9, 0.05, NULL,
                                  mqtt_wwd_threshold_callback);

          // ═══════════════════════════════════════════════════════════════════
          // AGC Controls
          // ═══════════════════════════════════════════════════════════════════
          mqtt_ha_register_switch("agc_enabled", "Auto Gain Control",
                                  mqtt_agc_switch_callback);
          mqtt_ha_register_number("agc_target_level", "AGC Target Level", 1000,
                                  8000, 500, NULL, mqtt_agc_target_callback);
          mqtt_ha_register_sensor("agc_current_gain", "AGC Current Gain", "x",
                                  NULL);
          // ═══════════════════════════════════════════════════════════════════

          // ═══════════════════════════════════════════════════════════════════
          // LED Status Controls
          // ═══════════════════════════════════════════════════════════════════
          mqtt_ha_register_switch("led_enabled", "LED Status Indicator",
                                  mqtt_led_switch_callback);
          mqtt_ha_register_number("led_brightness", "LED Brightness", 0, 100,
                                  10, "%", mqtt_led_brightness_callback);
          // ═══════════════════════════════════════════════════════════════════

          ESP_LOGI(TAG, "Home Assistant entities registered (17 sensors, 4 "
                        "switches, 9 buttons, 7 numbers)");

          // ═══════════════════════════════════════════════════════════════════
          // >>> NOVO: Inicijalni VA status <<<
          // ═══════════════════════════════════════════════════════════════════
          vTaskDelay(pdMS_TO_TICKS(500));
          mqtt_ha_update_sensor("va_status", "SPREMAN");
          mqtt_ha_update_sensor("va_response", "Voice Assistant spreman!");
          // ═══════════════════════════════════════════════════════════════════

          // ═══════════════════════════════════════════════════════════════════
          // Initialize AGC if enabled by default
          // ═══════════════════════════════════════════════════════════════════
          if (agc_enabled) {
            esp_err_t agc_ret = audio_capture_enable_agc(agc_target_level);
            if (agc_ret == ESP_OK) {
              ESP_LOGI(TAG, "AGC enabled with target level: %u",
                       agc_target_level);
              mqtt_ha_update_switch("agc_enabled", true);
            } else {
              ESP_LOGW(TAG, "Failed to enable AGC");
              agc_enabled = false;
              mqtt_ha_update_switch("agc_enabled", false);
            }
          }
          mqtt_ha_update_number("agc_target_level", (float)agc_target_level);
          // ═══════════════════════════════════════════════════════════════════

          // ═══════════════════════════════════════════════════════════════════
          // Initialize LED status
          // ═══════════════════════════════════════════════════════════════════
          mqtt_ha_update_switch("led_enabled", led_status_is_enabled());
          mqtt_ha_update_number("led_brightness", (float)led_status_get_brightness());
          ESP_LOGI(TAG, "LED status initialized: %s, brightness: %u%%",
                   led_status_is_enabled() ? "ON" : "OFF", led_status_get_brightness());
          // ═══════════════════════════════════════════════════════════════════

          xTaskCreate(mqtt_status_update_task, "mqtt_status", 4096, NULL, 3,
                      NULL);
          ESP_LOGI(TAG, "MQTT status update task started");
        } else {
          ESP_LOGW(TAG, "Failed to start MQTT client");
        }
      } else {
        ESP_LOGW(TAG, "Failed to initialize MQTT client");
      }

      ESP_LOGI(TAG, "All systems initialized - marking OTA partition as valid");
      ota_update_mark_valid();

      if (wwd_ret == ESP_OK && wwd_is_running() == false) {
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "🎙️  Voice Assistant Ready!");
        ESP_LOGI(TAG, "Wake Word Detection enabled");
        ESP_LOGI(TAG, "Say the wake word to activate!");
        ESP_LOGI(TAG, "Wake word: 'Hi ESP' (or your chosen model)");
        ESP_LOGI(TAG, "========================================");

        wwd_start();
        audio_capture_start_wake_word_mode(wwd_audio_feed_wrapper);

        // LED to green - wake word ready
        led_status_set(LED_STATUS_IDLE);
      } else {
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "🎙️  Voice Assistant Ready!");
        ESP_LOGI(TAG, "Using VAD-based activation");
        ESP_LOGI(TAG, "System will start recording in 5 seconds...");
        ESP_LOGI(TAG, "Just start speaking - VAD will detect automatically!");
        ESP_LOGI(TAG, "========================================");
        vTaskDelay(pdMS_TO_TICKS(5000));
        test_audio_streaming();
      }
    } else {
      ESP_LOGW(TAG, "Home Assistant connection failed");
    }
  } else {
    ESP_LOGW(TAG, "WiFi connection failed, continuing without network");
  }

  ESP_LOGI(TAG, "MP3 playback disabled (Voice Assistant mode)");
  ESP_LOGI(TAG, "Audio codec is ready for Voice Assistant development");
  ESP_LOGI(TAG, "System idle - ready to process voice commands...");

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}