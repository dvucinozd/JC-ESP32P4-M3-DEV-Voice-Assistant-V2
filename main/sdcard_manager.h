/**
 * @file sdcard_manager.h
 * @brief SD Card manager for ESP32-P4 Voice Assistant
 *
 * SD Card is only mounted when Ethernet is active (not WiFi fallback).
 * Uses SDMMC interface (4-bit mode) for high-speed access.
 *
 * Hardware: JC-ESP32P4-M3-DEV
 * - CLK: GPIO 43
 * - CMD: GPIO 44
 * - D0:  GPIO 39
 * - D1:  GPIO 40
 * - D2:  GPIO 41
 * - D3:  GPIO 42
 */

#ifndef SDCARD_MANAGER_H
#define SDCARD_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SD card information structure
 */
typedef struct {
    uint64_t capacity_bytes;    ///< Total capacity in bytes
    uint32_t capacity_mb;       ///< Total capacity in MB
    char type[16];              ///< Card type (SDSC, SDHC, SDXC)
    bool mounted;               ///< Mount status
} sdcard_info_t;

/**
 * @brief Initialize and mount SD card
 *
 * Mounts SD card to /sdcard using SDMMC 4-bit interface.
 * Only call this when Ethernet is active.
 *
 * @return ESP_OK on success
 *         ESP_FAIL if mount failed
 */
esp_err_t sdcard_manager_init(void);

/**
 * @brief Unmount and deinitialize SD card
 *
 * Call this when switching from Ethernet to WiFi fallback.
 *
 * @return ESP_OK on success
 */
esp_err_t sdcard_manager_deinit(void);

/**
 * @brief Check if SD card is mounted
 *
 * @return true if SD card is mounted and accessible
 *         false otherwise
 */
bool sdcard_manager_is_mounted(void);

/**
 * @brief Get SD card information
 *
 * @param info Pointer to sdcard_info_t structure to fill
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_STATE if card not mounted
 */
esp_err_t sdcard_manager_get_info(sdcard_info_t *info);

/**
 * @brief Get mount point path
 *
 * @return Mount point string ("/sdcard")
 */
const char* sdcard_manager_get_mount_point(void);

#ifdef __cplusplus
}
#endif

#endif // SDCARD_MANAGER_H
