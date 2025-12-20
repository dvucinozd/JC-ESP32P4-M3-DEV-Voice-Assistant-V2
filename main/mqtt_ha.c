/**
 * MQTT Home Assistant Integration Implementation
 */

#include "mqtt_ha.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "oled_status.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "mqtt_ha";

static void build_discovery_topic(const char *component, const char *entity_id,
                                  char *topic, size_t topic_len);

// Device information
#define DEVICE_NAME "ESP32-P4 Voice Assistant"
#define DEVICE_MODEL "JC-ESP32P4-M3-DEV"
#define DEVICE_MANUFACTURER "Guition"
#define DEVICE_ID "esp32p4_voice_assistant"

// MQTT topics
#define DISCOVERY_PREFIX "homeassistant"
#define STATE_PREFIX "esp32p4"

// Entity tracking
#define MAX_ENTITIES 40

typedef struct {
  char entity_id[32];
  char component[16];
  mqtt_ha_entity_type_t type;
  mqtt_ha_command_callback_t callback;
  char *discovery_payload; // retained discovery JSON (no secrets)
} mqtt_entity_t;

// Global state
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool mqtt_connected = false;
static mqtt_entity_t entities[MAX_ENTITIES];
static int entity_count = 0;
static bool legacy_cleanup_done = false;

#define LEGACY_DISCOVERY_COUNT 14
static const char *legacy_discovery_topics[LEGACY_DISCOVERY_COUNT] = {
    "homeassistant/button/esp32p4_voice_assistant/diag_dump/config",
    "homeassistant/button/esp32p4_voice_assistant/music_next/config",
    "homeassistant/button/esp32p4_voice_assistant/music_previous/config",
    "homeassistant/button/esp32p4_voice_assistant/music_resume/config",
    "homeassistant/button/esp32p4_voice_assistant/music_pause/config",
    "homeassistant/number/esp32p4_voice_assistant/vad_max_recording/config",
    "homeassistant/number/esp32p4_voice_assistant/vad_min_speech/config",
    "homeassistant/number/esp32p4_voice_assistant/vad_silence_duration/config",
    "homeassistant/number/esp32p4_voice_assistant/wwd_threshold/config",
    "homeassistant/sensor/esp32p4_voice_assistant/ota_url/config",
    "homeassistant/sensor/esp32p4_voice_assistant/webserial_clients/config",
    "homeassistant/switch/esp32p4_voice_assistant/agc_enabled/config",
    "homeassistant/switch/esp32p4_voice_assistant/led_enabled/config",
    "homeassistant/switch/esp32p4_voice_assistant/webserial_enabled/config"
};

static void mqtt_ha_cleanup_legacy_discovery(void) {
  if (!mqtt_client || legacy_cleanup_done) {
    return;
  }

  for (int i = 0; i < LEGACY_DISCOVERY_COUNT; i++) {
    int msg_id = esp_mqtt_client_publish(mqtt_client, legacy_discovery_topics[i],
                                         "", 0, 1, 1 /* retain */);
    if (msg_id >= 0) {
      ESP_LOGI(TAG, "Cleared legacy discovery: %s", legacy_discovery_topics[i]);
    } else {
      ESP_LOGW(TAG, "Failed to clear legacy discovery: %s", legacy_discovery_topics[i]);
    }
  }

  legacy_cleanup_done = true;
}

static int find_entity_index(const char *entity_id) {
  for (int i = 0; i < entity_count; i++) {
    if (strcmp(entities[i].entity_id, entity_id) == 0) {
      return i;
    }
  }
  return -1;
}

static esp_err_t publish_discovery_payload(const mqtt_entity_t *ent) {
  if (!mqtt_connected || !mqtt_client || !ent || !ent->discovery_payload ||
      !ent->component[0]) {
    return ESP_ERR_INVALID_STATE;
  }

  char topic[128];
  build_discovery_topic(ent->component, ent->entity_id, topic, sizeof(topic));

  int msg_id =
      esp_mqtt_client_publish(mqtt_client, topic, ent->discovery_payload, 0, 1,
                             1 /* retain */);
  return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

/**
 * Build Home Assistant MQTT Discovery topic
 * Format: homeassistant/<component>/<device_id>/<entity_id>/config
 */
static void build_discovery_topic(const char *component, const char *entity_id,
                                  char *topic, size_t topic_len) {
  snprintf(topic, topic_len, "%s/%s/%s/%s/config", DISCOVERY_PREFIX, component,
           DEVICE_ID, entity_id);
}

/**
 * Build state topic
 * Format: esp32p4/<entity_id>/state
 */
static void build_state_topic(const char *entity_id, char *topic,
                              size_t topic_len) {
  snprintf(topic, topic_len, "%s/%s/state", STATE_PREFIX, entity_id);
}

/**
 * Build command topic
 * Format: esp32p4/<entity_id>/set
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void build_command_topic(const char *entity_id, char *topic,
                                size_t topic_len) {
  snprintf(topic, topic_len, "%s/%s/set", STATE_PREFIX, entity_id);
}
#pragma GCC diagnostic pop

/**
 * Build device JSON object (shared across all entities)
 */
static cJSON *build_device_json(void) {
  cJSON *device = cJSON_CreateObject();

  cJSON *identifiers = cJSON_CreateArray();
  cJSON_AddItemToArray(identifiers, cJSON_CreateString(DEVICE_ID));
  cJSON_AddItemToObject(device, "identifiers", identifiers);

  cJSON_AddStringToObject(device, "name", DEVICE_NAME);
  cJSON_AddStringToObject(device, "model", DEVICE_MODEL);
  cJSON_AddStringToObject(device, "manufacturer", DEVICE_MANUFACTURER);
  const esp_app_desc_t *app_desc = esp_app_get_description();
  cJSON_AddStringToObject(device, "sw_version", app_desc ? app_desc->version : "unknown");

  return device;
}

/**
 * Publish MQTT Discovery message
 */
static esp_err_t publish_discovery(const char *component, const char *entity_id,
                                   cJSON *config) {
  if (!component || !entity_id || !config) {
    if (config) {
      cJSON_Delete(config);
    }
    return ESP_ERR_INVALID_ARG;
  }

  // Add device information
  cJSON_AddItemToObject(config, "device", build_device_json());

  // Convert to JSON string
  char *json_str = cJSON_PrintUnformatted(config);
  if (!json_str) {
    ESP_LOGE(TAG, "Failed to serialize discovery JSON");
    cJSON_Delete(config);
    return ESP_FAIL;
  }

  cJSON_Delete(config);

  int idx = find_entity_index(entity_id);
  if (idx < 0) {
    ESP_LOGW(TAG, "Discovery prepared for unknown entity: %s", entity_id);
    free(json_str);
    return ESP_FAIL;
  }

  strncpy(entities[idx].component, component, sizeof(entities[idx].component) - 1);
  entities[idx].component[sizeof(entities[idx].component) - 1] = '\0';

  if (entities[idx].discovery_payload) {
    free(entities[idx].discovery_payload);
  }
  entities[idx].discovery_payload = json_str;

  if (!mqtt_connected) {
    ESP_LOGI(TAG, "MQTT not connected yet; queued discovery for %s/%s", component,
             entity_id);
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Publishing discovery: %s/%s", component, entity_id);
  ESP_LOGD(TAG, "Discovery payload: %s", entities[idx].discovery_payload);
  return publish_discovery_payload(&entities[idx]);
}

/**
 * MQTT event handler
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

  switch (event->event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT connected to Home Assistant");
    mqtt_connected = true;
    oled_status_set_mqtt_connected(true);
    oled_status_set_last_event("mqtt-up");

    mqtt_ha_cleanup_legacy_discovery();

    // Republish discovery for all entities (handles boot-time race where
    // entities were registered before MQTT connected).
    for (int i = 0; i < entity_count; i++) {
      if (entities[i].discovery_payload && entities[i].component[0]) {
        (void)publish_discovery_payload(&entities[i]);
      }
    }

    // Subscribe to all command topics
    for (int i = 0; i < entity_count; i++) {
      if (entities[i].callback) {
        char topic[128];
        build_command_topic(entities[i].entity_id, topic, sizeof(topic));
        esp_mqtt_client_subscribe(mqtt_client, topic, 0);
        ESP_LOGI(TAG, "Subscribed to command topic: %s", topic);
      }
    }
    break;

  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "MQTT disconnected");
    mqtt_connected = false;
    oled_status_set_mqtt_connected(false);
    oled_status_set_last_event("mqtt-down");
    break;

  case MQTT_EVENT_DATA:
    ESP_LOGI(TAG, "MQTT message received: %.*s = %.*s", event->topic_len,
             event->topic, event->data_len, event->data);

    // Find matching entity and call callback
    char topic_buf[128];
    for (int i = 0; i < entity_count; i++) {
      if (entities[i].callback == NULL) {
        continue;
      }
      build_command_topic(entities[i].entity_id, topic_buf, sizeof(topic_buf));
      size_t topic_buf_len = strlen(topic_buf);

      // Compare both length and content
      if (event->topic_len == topic_buf_len &&
          strncmp(event->topic, topic_buf, event->topic_len) == 0) {
        // Null-terminate payload
        char payload[256];
        int len = (event->data_len < sizeof(payload) - 1) ? event->data_len
                                                          : sizeof(payload) - 1;
        memcpy(payload, event->data, len);
        payload[len] = '\0';

        ESP_LOGI(TAG, "Calling callback for entity: %s", entities[i].entity_id);
        entities[i].callback(entities[i].entity_id, payload);
        break;
      }
    }
    break;

  case MQTT_EVENT_ERROR:
    ESP_LOGE(TAG, "MQTT error");
    break;

  default:
    break;
  }
}

esp_err_t mqtt_ha_init(const mqtt_ha_config_t *config) {
  if (!config || !config->broker_uri) {
    ESP_LOGE(TAG, "Invalid MQTT configuration");
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGI(TAG, "Initializing MQTT Home Assistant client");
  ESP_LOGI(TAG, "Broker: %s", config->broker_uri);

  esp_mqtt_client_config_t mqtt_cfg = {
      .broker.address.uri = config->broker_uri,
      .credentials.client_id =
          config->client_id ? config->client_id : DEVICE_ID,
  };

  if (config->username) {
    mqtt_cfg.credentials.username = config->username;
  }
  if (config->password) {
    mqtt_cfg.credentials.authentication.password = config->password;
  }

  mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
  if (!mqtt_client) {
    ESP_LOGE(TAG, "Failed to initialize MQTT client");
    return ESP_FAIL;
  }

  esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                 mqtt_event_handler, NULL);

  ESP_LOGI(TAG, "MQTT Home Assistant client initialized");
  return ESP_OK;
}

esp_err_t mqtt_ha_start(void) {
  if (!mqtt_client) {
    ESP_LOGE(TAG, "MQTT client not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(TAG, "Starting MQTT client");
  esp_err_t err = esp_mqtt_client_start(mqtt_client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start MQTT client");
    return err;
  }

  return ESP_OK;
}

esp_err_t mqtt_ha_stop(void) {
  if (mqtt_client) {
    esp_mqtt_client_stop(mqtt_client);
    mqtt_connected = false;
  }
  return ESP_OK;
}

esp_err_t mqtt_ha_register_sensor(const char *entity_id, const char *name,
                                  const char *unit, const char *device_class) {
  if (entity_count >= MAX_ENTITIES) {
    ESP_LOGE(TAG, "Maximum entities reached");
    return ESP_ERR_NO_MEM;
  }

  // Store entity
  strncpy(entities[entity_count].entity_id, entity_id,
          sizeof(entities[entity_count].entity_id) - 1);
  entities[entity_count].type = MQTT_HA_SENSOR;
  entities[entity_count].callback = NULL;
  entity_count++;

  // Build discovery config
  cJSON *config = cJSON_CreateObject();
  cJSON_AddStringToObject(config, "name", name);

  char default_entity_id[64];
  snprintf(default_entity_id, sizeof(default_entity_id), "sensor.%s", entity_id);
  cJSON_AddStringToObject(config, "default_entity_id", default_entity_id);

  char unique_id[64];
  snprintf(unique_id, sizeof(unique_id), "%s_%s", DEVICE_ID, entity_id);
  cJSON_AddStringToObject(config, "unique_id", unique_id);

  char state_topic[64];
  build_state_topic(entity_id, state_topic, sizeof(state_topic));
  cJSON_AddStringToObject(config, "state_topic", state_topic);

  if (unit) {
    cJSON_AddStringToObject(config, "unit_of_measurement", unit);
  }
  if (device_class) {
    cJSON_AddStringToObject(config, "device_class", device_class);
  }

  return publish_discovery("sensor", entity_id, config);
}

esp_err_t mqtt_ha_register_switch(const char *entity_id, const char *name,
                                  mqtt_ha_command_callback_t callback) {
  if (entity_count >= MAX_ENTITIES) {
    ESP_LOGE(TAG, "Maximum entities reached");
    return ESP_ERR_NO_MEM;
  }

  // Store entity
  strncpy(entities[entity_count].entity_id, entity_id,
          sizeof(entities[entity_count].entity_id) - 1);
  entities[entity_count].type = MQTT_HA_SWITCH;
  entities[entity_count].callback = callback;
  entity_count++;

  // Build discovery config
  cJSON *config = cJSON_CreateObject();
  cJSON_AddStringToObject(config, "name", name);

  char default_entity_id[64];
  snprintf(default_entity_id, sizeof(default_entity_id), "switch.%s", entity_id);
  cJSON_AddStringToObject(config, "default_entity_id", default_entity_id);

  char unique_id[64];
  snprintf(unique_id, sizeof(unique_id), "%s_%s", DEVICE_ID, entity_id);
  cJSON_AddStringToObject(config, "unique_id", unique_id);

  char state_topic[64];
  build_state_topic(entity_id, state_topic, sizeof(state_topic));
  cJSON_AddStringToObject(config, "state_topic", state_topic);

  char command_topic[64];
  build_command_topic(entity_id, command_topic, sizeof(command_topic));
  cJSON_AddStringToObject(config, "command_topic", command_topic);

  esp_err_t ret = publish_discovery("switch", entity_id, config);

  // Subscribe to command topic if connected and callback is set
  if (ret == ESP_OK && mqtt_connected && callback) {
    esp_mqtt_client_subscribe(mqtt_client, command_topic, 0);
    ESP_LOGI(TAG, "Subscribed to command topic: %s", command_topic);
  }

  return ret;
}

esp_err_t mqtt_ha_register_number(const char *entity_id, const char *name,
                                  float min, float max, float step,
                                  const char *unit,
                                  mqtt_ha_command_callback_t callback) {
  if (entity_count >= MAX_ENTITIES) {
    ESP_LOGE(TAG, "Maximum entities reached");
    return ESP_ERR_NO_MEM;
  }

  // Store entity
  strncpy(entities[entity_count].entity_id, entity_id,
          sizeof(entities[entity_count].entity_id) - 1);
  entities[entity_count].type = MQTT_HA_NUMBER;
  entities[entity_count].callback = callback;
  entity_count++;

  // Build discovery config
  cJSON *config = cJSON_CreateObject();
  cJSON_AddStringToObject(config, "name", name);

  char default_entity_id[64];
  snprintf(default_entity_id, sizeof(default_entity_id), "number.%s", entity_id);
  cJSON_AddStringToObject(config, "default_entity_id", default_entity_id);

  char unique_id[64];
  snprintf(unique_id, sizeof(unique_id), "%s_%s", DEVICE_ID, entity_id);
  cJSON_AddStringToObject(config, "unique_id", unique_id);

  char state_topic[64];
  build_state_topic(entity_id, state_topic, sizeof(state_topic));
  cJSON_AddStringToObject(config, "state_topic", state_topic);

  char command_topic[64];
  build_command_topic(entity_id, command_topic, sizeof(command_topic));
  cJSON_AddStringToObject(config, "command_topic", command_topic);

  cJSON_AddNumberToObject(config, "min", min);
  cJSON_AddNumberToObject(config, "max", max);
  cJSON_AddNumberToObject(config, "step", step);

  if (unit) {
    cJSON_AddStringToObject(config, "unit_of_measurement", unit);
  }

  esp_err_t ret = publish_discovery("number", entity_id, config);

  // Subscribe to command topic if connected and callback is set
  if (ret == ESP_OK && mqtt_connected && callback) {
    esp_mqtt_client_subscribe(mqtt_client, command_topic, 0);
    ESP_LOGI(TAG, "Subscribed to command topic: %s", command_topic);
  }

  return ret;
}

esp_err_t mqtt_ha_register_select(const char *entity_id, const char *name,
                                  const char *options,
                                  mqtt_ha_command_callback_t callback) {
  if (entity_count >= MAX_ENTITIES) {
    ESP_LOGE(TAG, "Maximum entities reached");
    return ESP_ERR_NO_MEM;
  }

  // Store entity
  strncpy(entities[entity_count].entity_id, entity_id,
          sizeof(entities[entity_count].entity_id) - 1);
  entities[entity_count].type = MQTT_HA_SELECT;
  entities[entity_count].callback = callback;
  entity_count++;

  // Build discovery config
  cJSON *config = cJSON_CreateObject();
  cJSON_AddStringToObject(config, "name", name);

  char default_entity_id[64];
  snprintf(default_entity_id, sizeof(default_entity_id), "select.%s", entity_id);
  cJSON_AddStringToObject(config, "default_entity_id", default_entity_id);

  char unique_id[64];
  snprintf(unique_id, sizeof(unique_id), "%s_%s", DEVICE_ID, entity_id);
  cJSON_AddStringToObject(config, "unique_id", unique_id);

  char state_topic[64];
  build_state_topic(entity_id, state_topic, sizeof(state_topic));
  cJSON_AddStringToObject(config, "state_topic", state_topic);

  char command_topic[64];
  build_command_topic(entity_id, command_topic, sizeof(command_topic));
  cJSON_AddStringToObject(config, "command_topic", command_topic);

  // Parse options (comma-separated)
  cJSON *options_array = cJSON_CreateArray();
  char options_buf[256];
  strncpy(options_buf, options, sizeof(options_buf) - 1);
  char *token = strtok(options_buf, ",");
  while (token) {
    cJSON_AddItemToArray(options_array, cJSON_CreateString(token));
    token = strtok(NULL, ",");
  }
  cJSON_AddItemToObject(config, "options", options_array);

  esp_err_t ret = publish_discovery("select", entity_id, config);

  // Subscribe to command topic if connected and callback is set
  if (ret == ESP_OK && mqtt_connected && callback) {
    esp_mqtt_client_subscribe(mqtt_client, command_topic, 0);
    ESP_LOGI(TAG, "Subscribed to command topic: %s", command_topic);
  }

  return ret;
}

esp_err_t mqtt_ha_register_button(const char *entity_id, const char *name,
                                  mqtt_ha_command_callback_t callback) {
  if (entity_count >= MAX_ENTITIES) {
    ESP_LOGE(TAG, "Maximum entities reached");
    return ESP_ERR_NO_MEM;
  }

  // Store entity
  strncpy(entities[entity_count].entity_id, entity_id,
          sizeof(entities[entity_count].entity_id) - 1);
  entities[entity_count].type = MQTT_HA_BUTTON;
  entities[entity_count].callback = callback;
  entity_count++;

  // Build discovery config
  cJSON *config = cJSON_CreateObject();
  cJSON_AddStringToObject(config, "name", name);

  char default_entity_id[64];
  snprintf(default_entity_id, sizeof(default_entity_id), "button.%s", entity_id);
  cJSON_AddStringToObject(config, "default_entity_id", default_entity_id);

  char unique_id[64];
  snprintf(unique_id, sizeof(unique_id), "%s_%s", DEVICE_ID, entity_id);
  cJSON_AddStringToObject(config, "unique_id", unique_id);

  char command_topic[64];
  build_command_topic(entity_id, command_topic, sizeof(command_topic));
  cJSON_AddStringToObject(config, "command_topic", command_topic);

  esp_err_t ret = publish_discovery("button", entity_id, config);

  // Subscribe to command topic if connected and callback is set
  if (ret == ESP_OK && mqtt_connected && callback) {
    esp_mqtt_client_subscribe(mqtt_client, command_topic, 0);
    ESP_LOGI(TAG, "Subscribed to command topic: %s", command_topic);
  }

  return ret;
}

esp_err_t mqtt_ha_update_sensor(const char *entity_id, const char *value) {
  if (!mqtt_connected) {
    return ESP_ERR_INVALID_STATE;
  }

  char topic[64];
  build_state_topic(entity_id, topic, sizeof(topic));

  int msg_id = esp_mqtt_client_publish(mqtt_client, topic, value, 0, 1, 0);
  return (msg_id >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_ha_update_switch(const char *entity_id, bool state) {
  return mqtt_ha_update_sensor(entity_id, state ? "ON" : "OFF");
}

esp_err_t mqtt_ha_update_number(const char *entity_id, float value) {
  char value_str[32];
  snprintf(value_str, sizeof(value_str), "%.2f", value);
  return mqtt_ha_update_sensor(entity_id, value_str);
}

esp_err_t mqtt_ha_update_select(const char *entity_id, const char *option) {
  return mqtt_ha_update_sensor(entity_id, option);
}

esp_err_t mqtt_ha_register_text(const char *entity_id, const char *name,
                                mqtt_ha_command_callback_t callback) {
  if (entity_count >= MAX_ENTITIES) {
    ESP_LOGE(TAG, "Maximum entities reached");
    return ESP_ERR_NO_MEM;
  }

  // Store entity
  strncpy(entities[entity_count].entity_id, entity_id,
          sizeof(entities[entity_count].entity_id) - 1);
  entities[entity_count].type = MQTT_HA_SENSOR; // Use sensor type for text
  entities[entity_count].callback = callback;
  entity_count++;

  // Build discovery config for text entity
  cJSON *config = cJSON_CreateObject();
  cJSON_AddStringToObject(config, "name", name);

  char default_entity_id[64];
  snprintf(default_entity_id, sizeof(default_entity_id), "text.%s", entity_id);
  cJSON_AddStringToObject(config, "default_entity_id", default_entity_id);

  char unique_id[64];
  snprintf(unique_id, sizeof(unique_id), "%s_%s", DEVICE_ID, entity_id);
  cJSON_AddStringToObject(config, "unique_id", unique_id);

  char state_topic[64];
  build_state_topic(entity_id, state_topic, sizeof(state_topic));
  cJSON_AddStringToObject(config, "state_topic", state_topic);

  char command_topic[64];
  build_command_topic(entity_id, command_topic, sizeof(command_topic));
  cJSON_AddStringToObject(config, "command_topic", command_topic);

  // Text mode (not password)
  cJSON_AddStringToObject(config, "mode", "text");

  // Max length for URL
  cJSON_AddNumberToObject(config, "max", 255);

  // Subscribe to command topic
  if (mqtt_connected && callback) {
    esp_mqtt_client_subscribe(mqtt_client, command_topic, 0);
    ESP_LOGI(TAG, "Subscribed to text command: %s", command_topic);
  }

  return publish_discovery("text", entity_id, config);
}

esp_err_t mqtt_ha_update_text(const char *entity_id, const char *value) {
  return mqtt_ha_update_sensor(entity_id, value);
}

bool mqtt_ha_is_connected(void) { return mqtt_connected; }
