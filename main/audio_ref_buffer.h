#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the reference audio buffer
 * @param size Buffer size in bytes
 */
esp_err_t audio_ref_buffer_init(size_t size);

/**
 * @brief Write playback audio to reference buffer (Push)
 * Called by player before writing to I2S.
 * 
 * @param data Audio data (16-bit PCM)
 * @param len Length in bytes
 */
void audio_ref_buffer_write(const void *data, size_t len);

/**
 * @brief Read reference audio for AFE (Pop)
 * Called by audio_capture to get reference signal.
 * 
 * @param dest Buffer to read into
 * @param len Number of bytes to read
 * @return Number of bytes actually read (fills rest with 0)
 */
size_t audio_ref_buffer_read(void *dest, size_t len);

#ifdef __cplusplus
}
#endif
