/**
 * Home Assistant Client
 * Handles WebSocket connection to Home Assistant for Voice Assistant
 */

#ifndef HA_CLIENT_H
#define HA_CLIENT_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *hostname;
    int port;
    const char *access_token;
    bool use_ssl;
} ha_client_config_t;

/**
 * @brief Initialize Home Assistant client
 *
 * This function:
 * - Resolves HA hostname via mDNS (kira.local)
 * - Establishes WebSocket connection over HTTPS
 * - Authenticates using long-lived access token
 * - Subscribes to events
 *
 * @param config Configuration struct
 * @return
 *    - ESP_OK: Successfully connected to Home Assistant
 *    - ESP_FAIL: Connection failed
 */
esp_err_t ha_client_init(const ha_client_config_t *config);

// Wrapper for backward compatibility or simple use
static inline esp_err_t ha_client_init_legacy(void) {
    return ESP_ERR_NOT_SUPPORTED; 
}

/**
 * @brief Check if connected to Home Assistant
 *
 * @return true if WebSocket is connected and authenticated
 */
bool ha_client_is_connected(void);

/**
 * @brief Check if HA is ready to accept binary audio frames
 *
 * Audio streaming becomes ready after the Assist pipeline run-start event
 * provides `stt_binary_handler_id`.
 */
bool ha_client_is_audio_ready(void);

/**
 * @brief Get the current STT binary handler id (or -1 if not ready).
 */
int ha_client_get_stt_binary_handler_id(void);

/**
 * @brief Send text to Home Assistant Assist Pipeline
 *
 * @param text Text to process (voice transcription)
 * @return ESP_OK on success
 */
esp_err_t ha_client_send_text(const char *text);

/**
 * @brief Start voice assistant conversation
 *
 * Initiates a new conversation with HA Assist Pipeline.
 * After this, audio can be streamed.
 *
 * @return conversation_id (allocated string, caller must free)
 */
char* ha_client_start_conversation(void);

/**
 * @brief Stream audio data to Home Assistant
 *
 * @param audio_data PCM audio buffer
 * @param length Length of audio data in bytes
 * @param conversation_id Conversation ID from start_conversation
 * @return ESP_OK on success
 */
esp_err_t ha_client_stream_audio(const uint8_t *audio_data, size_t length, const char *conversation_id);

/**
 * @brief End audio streaming (signals recording is complete)
 *
 * @return ESP_OK on success
 */
esp_err_t ha_client_end_audio_stream(void);

/**
 * @brief Callback for conversation responses from HA
 *
 * @param response_text TTS response text from Home Assistant
 * @param conversation_id Associated conversation ID
 */
typedef void (*ha_conversation_callback_t)(const char *response_text, const char *conversation_id);

/**
 * @brief Callback for TTS audio data from HA
 *
 * @param audio_data TTS audio chunk (MP3/WAV format)
 * @param length Length of audio data in bytes
 */
typedef void (*ha_tts_audio_callback_t)(const uint8_t *audio_data, size_t length);

/**
 * @brief Callback for pipeline errors or unexpected termination
 *
 * @param error_code Error code string from HA
 * @param error_message Error message from HA
 */
typedef void (*ha_pipeline_error_callback_t)(const char *error_code, const char *error_message);

/**
 * @brief Callback for intent recognition results
 *
 * @param intent_name Name of the recognized intent (e.g., "SetTimer", "CancelTimer")
 * @param intent_data JSON string with intent parameters/slots (caller must NOT free)
 * @param conversation_id Associated conversation ID
 */
typedef void (*ha_intent_callback_t)(const char *intent_name, const char *intent_data, const char *conversation_id);

/**
 * @brief Callback for STT text output
 *
 * @param text Recognized text from STT
 * @param conversation_id Associated conversation ID
 */
typedef void (*ha_stt_callback_t)(const char *text, const char *conversation_id);

/**
 * @brief Register callback for conversation responses
 *
 * @param callback Function to call when HA sends conversation response
 */
void ha_client_register_conversation_callback(ha_conversation_callback_t callback);

/**
 * @brief Register callback for TTS audio data
 *
 * @param callback Function to call when HA sends TTS audio chunk
 */
void ha_client_register_tts_audio_callback(ha_tts_audio_callback_t callback);

/**
 * @brief Register callback for pipeline errors
 *
 * @param callback Function to call when pipeline fails
 */
void ha_client_register_error_callback(ha_pipeline_error_callback_t callback);

/**
 * @brief Register callback for intent recognition
 *
 * @param callback Function to call when intent is recognized
 */
void ha_client_register_intent_callback(ha_intent_callback_t callback);

/**
 * @brief Register callback for STT output text
 *
 * @param callback Function to call when STT text is available
 */
void ha_client_register_stt_callback(ha_stt_callback_t callback);

/**
 * @brief Stop Home Assistant client and disconnect
 */
void ha_client_stop(void);

/**
 * @brief Request TTS for test message
 *
 * Sends a conversation/process request to HA to synthesize test audio.
 * The TTS audio will be delivered via the registered TTS audio callback.
 *
 * @param text Text to synthesize (e.g., "Test audio output")
 * @return ESP_OK on success
 */
esp_err_t ha_client_request_tts(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* HA_CLIENT_H */
