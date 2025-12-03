/**
 * @file sdcard_manager.c
 * @brief SD Card manager implementation
 */

#include "sdcard_manager.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include <string.h>

static const char *TAG = "sdcard_manager";

// SD card configuration for JC-ESP32P4-M3-DEV
#define SDCARD_MOUNT_POINT  "/sdcard"
#define SDCARD_CLK_GPIO     43
#define SDCARD_CMD_GPIO     44
#define SDCARD_D0_GPIO      39
#define SDCARD_D1_GPIO      40
#define SDCARD_D2_GPIO      41
#define SDCARD_D3_GPIO      42

// SD card state
static bool sd_card_mounted = false;
static sdmmc_card_t *sd_card = NULL;

/**
 * @brief Initialize and mount SD card
 */
esp_err_t sdcard_manager_init(void)
{
    if (sd_card_mounted) {
        ESP_LOGW(TAG, "SD card already mounted");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SD card (SDMMC 4-bit mode)...");
    ESP_LOGI(TAG, "Pins: CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d",
             SDCARD_CLK_GPIO, SDCARD_CMD_GPIO,
             SDCARD_D0_GPIO, SDCARD_D1_GPIO, SDCARD_D2_GPIO, SDCARD_D3_GPIO);

    // Configure SDMMC host
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_4BIT;  // Use 4-bit mode for higher speed
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  // High-speed mode

    // Configure slot
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SDCARD_CLK_GPIO;
    slot_config.cmd = SDCARD_CMD_GPIO;
    slot_config.d0 = SDCARD_D0_GPIO;
    slot_config.d1 = SDCARD_D1_GPIO;
    slot_config.d2 = SDCARD_D2_GPIO;
    slot_config.d3 = SDCARD_D3_GPIO;
    slot_config.width = 4;  // 4-bit data bus
    slot_config.cd = SDMMC_SLOT_NO_CD;  // No card detect pin
    slot_config.wp = SDMMC_SLOT_NO_WP;  // No write protect pin
    slot_config.flags = 0;

    // Mount configuration
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,  // Don't format on failure
        .max_files = 10,  // Max open files
        .allocation_unit_size = 64 * 1024,  // 64KB clusters for better performance
        .disk_status_check_enable = false
    };

    // Mount SD card
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot_config,
                                             &mount_config, &sd_card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                     "If you want to format the card, set format_if_mount_failed = true.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    sd_card_mounted = true;

    // Print card information
    sdmmc_card_print_info(stdout, sd_card);

    uint64_t capacity_bytes = ((uint64_t)sd_card->csd.capacity) * sd_card->csd.sector_size;
    uint32_t capacity_mb = capacity_bytes / (1024 * 1024);

    ESP_LOGI(TAG, "✅ SD card mounted successfully!");
    ESP_LOGI(TAG, "   Name: %s", sd_card->cid.name);
    ESP_LOGI(TAG, "   Type: %s",
             (sd_card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC");
    ESP_LOGI(TAG, "   Speed: %s",
             (sd_card->csd.tr_speed > 25000000) ? "High Speed" : "Default Speed");
    ESP_LOGI(TAG, "   Capacity: %lu MB (%.2f GB)",
             (unsigned long)capacity_mb, capacity_bytes / (1024.0 * 1024.0 * 1024.0));
    ESP_LOGI(TAG, "   Mount point: %s", SDCARD_MOUNT_POINT);

    return ESP_OK;
}

/**
 * @brief Unmount and deinitialize SD card
 */
esp_err_t sdcard_manager_deinit(void)
{
    if (!sd_card_mounted) {
        ESP_LOGW(TAG, "SD card not mounted");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Unmounting SD card...");

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, sd_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount SD card: %s", esp_err_to_name(ret));
        return ret;
    }

    sd_card = NULL;
    sd_card_mounted = false;

    ESP_LOGI(TAG, "SD card unmounted");
    return ESP_OK;
}

/**
 * @brief Check if SD card is mounted
 */
bool sdcard_manager_is_mounted(void)
{
    return sd_card_mounted;
}

/**
 * @brief Get SD card information
 */
esp_err_t sdcard_manager_get_info(sdcard_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!sd_card_mounted || !sd_card) {
        return ESP_ERR_INVALID_STATE;
    }

    info->capacity_bytes = ((uint64_t)sd_card->csd.capacity) * sd_card->csd.sector_size;
    info->capacity_mb = info->capacity_bytes / (1024 * 1024);

    if (sd_card->ocr & SD_OCR_SDHC_CAP) {
        strncpy(info->type, "SDHC/SDXC", sizeof(info->type) - 1);
    } else {
        strncpy(info->type, "SDSC", sizeof(info->type) - 1);
    }
    info->type[sizeof(info->type) - 1] = '\0';

    info->mounted = sd_card_mounted;

    return ESP_OK;
}

/**
 * @brief Get mount point path
 */
const char* sdcard_manager_get_mount_point(void)
{
    return SDCARD_MOUNT_POINT;
}
