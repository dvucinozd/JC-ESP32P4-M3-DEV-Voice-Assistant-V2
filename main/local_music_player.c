/**
 * @file local_music_player.c
 * @brief Local music player implementation
 */

#include "local_music_player.h"
#include "bsp_board_extra.h"
#include "bsp/esp32_p4_function_ev_board.h"
#include "file_iterator.h"
#include "audio_player.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "driver/i2s_std.h"

static const char *TAG = "local_music";

// External function from BSP to reconfigure codec sample rate
extern esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch);

#define MUSIC_DIR "/sdcard/music"

// Player state
static bool player_initialized = false;
static music_state_t player_state = MUSIC_STATE_IDLE;
static file_iterator_instance_t *file_iterator = NULL;
static int current_track_index = -1;
static int total_tracks = 0;
static music_event_callback_t event_callback = NULL;
static bool manual_stop = false;  // Flag to prevent auto-play after manual stop

static void free_file_iterator_instance(file_iterator_instance_t **instance)
{
    if (instance == NULL || *instance == NULL) {
        return;
    }

    file_iterator_instance_t *it = *instance;

    if (it->list) {
        for (size_t i = 0; i < it->count; i++) {
            free(it->list[i]);
        }
        free(it->list);
    }

    if (it->directory_path) {
        free((void *)it->directory_path);
    }

    free(it);
    *instance = NULL;
}

/**
 * @brief Audio player callback from BSP
 */
static void audio_player_callback(audio_player_cb_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Audio player event: %d", ctx->audio_event);

    switch (ctx->audio_event) {
        case AUDIO_PLAYER_CALLBACK_EVENT_IDLE:
        case AUDIO_PLAYER_CALLBACK_EVENT_COMPLETED_PLAYING_NEXT:
            // Track finished - only auto-play next if not manually stopped
            if (manual_stop) {
                ESP_LOGI(TAG, "Track stopped manually - staying stopped");
                manual_stop = false;  // Reset flag
            } else {
                // Check if this was the last track
                if (current_track_index == total_tracks - 1) {
                    ESP_LOGI(TAG, "Last track finished - stopping playback");
                    player_state = MUSIC_STATE_STOPPED;
                    if (event_callback) {
                        event_callback(player_state, current_track_index, total_tracks);
                    }
                } else {
                    ESP_LOGI(TAG, "Track finished, playing next...");
                    local_music_player_next();
                }
            }
            break;

        case AUDIO_PLAYER_CALLBACK_EVENT_PLAYING:
            player_state = MUSIC_STATE_PLAYING;
            if (event_callback) {
                event_callback(player_state, current_track_index, total_tracks);
            }
            break;

        case AUDIO_PLAYER_CALLBACK_EVENT_PAUSE:
            player_state = MUSIC_STATE_PAUSED;
            if (event_callback) {
                event_callback(player_state, current_track_index, total_tracks);
            }
            break;

        case AUDIO_PLAYER_CALLBACK_EVENT_SHUTDOWN:
            player_state = MUSIC_STATE_STOPPED;
            if (event_callback) {
                event_callback(player_state, current_track_index, total_tracks);
            }
            break;

        default:
            break;
    }
}

/**
 * @brief Initialize local music player
 */
esp_err_t local_music_player_init(void)
{
    if (player_initialized) {
        ESP_LOGW(TAG, "Music player already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing local music player...");
    ESP_LOGI(TAG, "Music directory: %s", MUSIC_DIR);

    // Check if SD card is mounted
    if (bsp_sdcard == NULL) {
        ESP_LOGE(TAG, "SD card not mounted - cannot initialize music player");
        return ESP_FAIL;
    }

    // Initialize BSP audio player
    esp_err_t ret = bsp_extra_player_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BSP audio player: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register audio player callback
    bsp_extra_player_register_callback(audio_player_callback, NULL);

    // Initialize file iterator for music directory
    file_iterator = file_iterator_new(MUSIC_DIR);
    if (file_iterator == NULL) {
        ESP_LOGE(TAG, "Failed to initialize file iterator for %s", MUSIC_DIR);
        bsp_extra_player_del();
        return ESP_FAIL;
    }

    // Count total tracks
    total_tracks = file_iterator->count;
    ESP_LOGI(TAG, "Found %d music tracks in %s", total_tracks, MUSIC_DIR);

    if (total_tracks == 0) {
        ESP_LOGW(TAG, "No music files found in %s", MUSIC_DIR);
        bsp_extra_player_del();
        free_file_iterator_instance(&file_iterator);
        return ESP_FAIL;
    }

    player_initialized = true;
    player_state = MUSIC_STATE_IDLE;
    current_track_index = -1;

    ESP_LOGI(TAG, "Local music player initialized successfully");
    ESP_LOGI(TAG, "Total tracks: %d", total_tracks);

    return ESP_OK;
}

/**
 * @brief Deinitialize local music player
 */
esp_err_t local_music_player_deinit(void)
{
    if (!player_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing local music player...");

    // Stop playback if active
    if (player_state == MUSIC_STATE_PLAYING || player_state == MUSIC_STATE_PAUSED) {
        local_music_player_stop();
    }

    // Delete BSP audio player
    bsp_extra_player_del();

    // Delete file iterator (manual cleanup since file_iterator_delete doesn't exist)
    free_file_iterator_instance(&file_iterator);
    total_tracks = 0;
    current_track_index = -1;
    player_state = MUSIC_STATE_IDLE;
    player_initialized = false;

    ESP_LOGI(TAG, "Local music player deinitialized");

    return ESP_OK;
}

/**
 * @brief Start playing music
 */
esp_err_t local_music_player_play(void)
{
    if (!player_initialized) {
        ESP_LOGE(TAG, "Music player not initialized");
        return ESP_FAIL;
    }

    if (total_tracks == 0) {
        ESP_LOGE(TAG, "No tracks available");
        return ESP_FAIL;
    }

    // If paused, resume
    if (player_state == MUSIC_STATE_PAUSED) {
        return local_music_player_resume();
    }

    // Start from first track
    current_track_index = 0;
    ESP_LOGI(TAG, "Starting playback from track %d/%d",
             current_track_index + 1, total_tracks);

    // Clear manual stop flag - user explicitly pressed play
    manual_stop = false;

    // Reconfigure codec for MP3 playback (48kHz stereo for your MP3 files)
    // This is critical after voice recording which uses 16kHz mono
    ESP_LOGI(TAG, "Configuring codec for music playback (48kHz stereo)");
    esp_err_t codec_ret = bsp_extra_codec_set_fs(48000, 16, I2S_SLOT_MODE_STEREO);
    if (codec_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reconfigure codec, music may play at wrong speed");
    }

    esp_err_t ret = bsp_extra_player_play_index(file_iterator, current_track_index);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to play track %d", current_track_index);
        return ret;
    }

    player_state = MUSIC_STATE_PLAYING;

    if (event_callback) {
        event_callback(player_state, current_track_index, total_tracks);
    }

    return ESP_OK;
}

/**
 * @brief Stop playing music
 */
esp_err_t local_music_player_stop(void)
{
    if (!player_initialized) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Stopping music playback (manual stop)");

    // Set manual stop flag to prevent auto-play next track
    manual_stop = true;

    // Use audio player stop (queues stop request)
    esp_err_t ret = audio_player_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop audio player");
        manual_stop = false;  // Reset on error
        return ret;
    }

    player_state = MUSIC_STATE_STOPPED;
    current_track_index = -1;

    if (event_callback) {
        event_callback(player_state, current_track_index, total_tracks);
    }

    return ESP_OK;
}

/**
 * @brief Pause playing music
 */
esp_err_t local_music_player_pause(void)
{
    if (!player_initialized || player_state != MUSIC_STATE_PLAYING) {
        ESP_LOGW(TAG, "Cannot pause - not playing");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Pausing music playback");

    // Use audio player pause (queues pause request)
    esp_err_t ret = audio_player_pause();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to pause audio player");
        return ret;
    }

    player_state = MUSIC_STATE_PAUSED;

    if (event_callback) {
        event_callback(player_state, current_track_index, total_tracks);
    }

    return ESP_OK;
}

/**
 * @brief Resume playing music
 */
esp_err_t local_music_player_resume(void)
{
    if (!player_initialized || player_state != MUSIC_STATE_PAUSED) {
        ESP_LOGW(TAG, "Cannot resume - not paused");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Resuming music playback");

    // Clear manual stop flag - user pressed resume
    manual_stop = false;

    // Ensure codec is back to music playback settings (TTS/WWD may have changed it)
    ESP_LOGI(TAG, "Configuring codec for music playback (48kHz stereo)");
    esp_err_t codec_ret = bsp_extra_codec_set_fs(48000, 16, I2S_SLOT_MODE_STEREO);
    if (codec_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reconfigure codec");
    }

    // Use audio player resume (queues resume request)
    esp_err_t ret = audio_player_resume();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to resume audio player");
        return ret;
    }

    player_state = MUSIC_STATE_PLAYING;

    if (event_callback) {
        event_callback(player_state, current_track_index, total_tracks);
    }

    return ESP_OK;
}

/**
 * @brief Play next track
 */
esp_err_t local_music_player_next(void)
{
    if (!player_initialized || total_tracks == 0) {
        return ESP_FAIL;
    }

    // Increment track index (loop to beginning)
    current_track_index = (current_track_index + 1) % total_tracks;

    ESP_LOGI(TAG, "Playing next track: %d/%d", current_track_index + 1, total_tracks);

    // Clear manual stop flag - user pressed next or track auto-advanced
    manual_stop = false;

    // Reconfigure codec for MP3 playback
    ESP_LOGI(TAG, "Configuring codec for music playback (48kHz stereo)");
    esp_err_t codec_ret = bsp_extra_codec_set_fs(48000, 16, I2S_SLOT_MODE_STEREO);
    if (codec_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reconfigure codec");
    }

    esp_err_t ret = bsp_extra_player_play_index(file_iterator, current_track_index);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to play next track");
        return ret;
    }

    player_state = MUSIC_STATE_PLAYING;

    if (event_callback) {
        event_callback(player_state, current_track_index, total_tracks);
    }

    return ESP_OK;
}

/**
 * @brief Play previous track
 */
esp_err_t local_music_player_previous(void)
{
    if (!player_initialized || total_tracks == 0) {
        return ESP_FAIL;
    }

    // Decrement track index (loop to end)
    current_track_index--;
    if (current_track_index < 0) {
        current_track_index = total_tracks - 1;
    }

    ESP_LOGI(TAG, "Playing previous track: %d/%d", current_track_index + 1, total_tracks);

    // Clear manual stop flag - user pressed previous
    manual_stop = false;

    // Reconfigure codec for MP3 playback
    ESP_LOGI(TAG, "Configuring codec for music playback (48kHz stereo)");
    esp_err_t codec_ret = bsp_extra_codec_set_fs(48000, 16, I2S_SLOT_MODE_STEREO);
    if (codec_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reconfigure codec");
    }

    esp_err_t ret = bsp_extra_player_play_index(file_iterator, current_track_index);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to play previous track");
        return ret;
    }

    player_state = MUSIC_STATE_PLAYING;

    if (event_callback) {
        event_callback(player_state, current_track_index, total_tracks);
    }

    return ESP_OK;
}

/**
 * @brief Play specific track by index
 */
esp_err_t local_music_player_play_track(int track_index)
{
    if (!player_initialized) {
        ESP_LOGE(TAG, "Music player not initialized");
        return ESP_FAIL;
    }

    if (track_index < 0 || track_index >= total_tracks) {
        ESP_LOGE(TAG, "Invalid track index: %d (total: %d)", track_index, total_tracks);
        return ESP_ERR_INVALID_ARG;
    }

    current_track_index = track_index;

    ESP_LOGI(TAG, "Playing track %d/%d", current_track_index + 1, total_tracks);

    esp_err_t ret = bsp_extra_player_play_index(file_iterator, current_track_index);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to play track %d", current_track_index);
        return ret;
    }

    player_state = MUSIC_STATE_PLAYING;

    if (event_callback) {
        event_callback(player_state, current_track_index, total_tracks);
    }

    return ESP_OK;
}

/**
 * @brief Get current player state
 */
music_state_t local_music_player_get_state(void)
{
    return player_state;
}

/**
 * @brief Get current track index
 */
int local_music_player_get_current_track(void)
{
    return current_track_index;
}

/**
 * @brief Get total number of tracks
 */
int local_music_player_get_total_tracks(void)
{
    return total_tracks;
}

/**
 * @brief Get current track name
 */
esp_err_t local_music_player_get_track_name(char *name, size_t max_len)
{
    if (!player_initialized || !name || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (current_track_index < 0 || current_track_index >= total_tracks) {
        snprintf(name, max_len, "No track");
        return ESP_FAIL;
    }

    if (file_iterator && file_iterator->list) {
        // Get filename from iterator
        const char *filename = file_iterator_get_name_from_index(file_iterator, current_track_index);
        if (filename) {
            strncpy(name, filename, max_len - 1);
            name[max_len - 1] = '\0';
            return ESP_OK;
        }
    }

    snprintf(name, max_len, "Track %d", current_track_index + 1);
    return ESP_OK;
}

/**
 * @brief Check if player is initialized
 */
bool local_music_player_is_initialized(void)
{
    return player_initialized;
}

/**
 * @brief Register event callback
 */
void local_music_player_register_callback(music_event_callback_t callback)
{
    event_callback = callback;
    ESP_LOGI(TAG, "Music player event callback registered");
}
