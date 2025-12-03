/**
 * @file local_music_player.c
 * @brief Local music player implementation
 */

#include "local_music_player.h"
#include "bsp_board_extra.h"
#include "bsp/esp32_p4_function_ev_board.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "local_music";

#define MUSIC_DIR "/sdcard/music"

// Player state
static bool player_initialized = false;
static music_state_t player_state = MUSIC_STATE_IDLE;
static file_iterator_instance_t *file_iterator = NULL;
static int current_track_index = -1;
static int total_tracks = 0;
static music_event_callback_t event_callback = NULL;

/**
 * @brief Audio player callback from BSP
 */
static void audio_player_callback(audio_player_cb_ctx_t *ctx)
{
    ESP_LOGI(TAG, "Audio player event: %d", ctx->audio_event);

    switch (ctx->audio_event) {
        case AUDIO_PLAYER_CALLBACK_EVENT_IDLE:
        case AUDIO_PLAYER_CALLBACK_EVENT_COMPLETED_PLAYING_NEXT:
            // Track finished, play next automatically
            ESP_LOGI(TAG, "Track finished, playing next...");
            local_music_player_next();
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
    ret = bsp_extra_file_instance_init(MUSIC_DIR, &file_iterator);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize file iterator for %s: %s",
                 MUSIC_DIR, esp_err_to_name(ret));
        bsp_extra_player_del();
        return ret;
    }

    if (file_iterator == NULL) {
        ESP_LOGE(TAG, "File iterator is NULL");
        bsp_extra_player_del();
        return ESP_FAIL;
    }

    // Count total tracks
    total_tracks = file_iterator->file_num;
    ESP_LOGI(TAG, "Found %d music tracks in %s", total_tracks, MUSIC_DIR);

    if (total_tracks == 0) {
        ESP_LOGW(TAG, "No music files found in %s", MUSIC_DIR);
        bsp_extra_player_del();
        file_iterator = NULL;
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
    if (player_state == MUSIC_STATE_PLAYING) {
        local_music_player_stop();
    }

    // Delete BSP audio player
    bsp_extra_player_del();

    file_iterator = NULL;
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

    ESP_LOGI(TAG, "Stopping music playback");

    // BSP doesn't have explicit stop, so we deinit and reinit
    bsp_extra_player_del();
    bsp_extra_player_init();
    bsp_extra_player_register_callback(audio_player_callback, NULL);

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

    // BSP audio player handles pause internally via codec
    bsp_extra_codec_dev_stop();

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

    bsp_extra_codec_dev_resume();

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

    if (file_iterator && file_iterator->file_name) {
        // Get filename from iterator
        char **file_list = file_iterator->file_name;
        if (file_list[current_track_index]) {
            strncpy(name, file_list[current_track_index], max_len - 1);
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
