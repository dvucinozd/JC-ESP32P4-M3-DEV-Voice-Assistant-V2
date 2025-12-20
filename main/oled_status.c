#include "oled_status.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp32_p4_function_ev_board.h"
#include "bsp_board_extra.h"
#include "driver/i2c.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "config.h"
#include "ha_client.h"
#include "led_status.h"
#include "mqtt_ha.h"
#include "network_manager.h"
#include "sys_diag.h"
#include "va_control.h"

#define TAG "oled_status"

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_PAGES (OLED_HEIGHT / 8)
#define OLED_FB_SIZE (OLED_WIDTH * OLED_PAGES)

#define OLED_ADDR_PRIMARY 0x3C
#define OLED_ADDR_FALLBACK 0x3D

#define OLED_REFRESH_MIN_MS 200
#define OLED_PAGE_ROTATE_MS 2500
#define OLED_I2C_TIMEOUT_MS 25
#define OLED_I2C_SPEED_HZ 100000

typedef struct {
    bool enabled;
    bool safe_mode;
    bool ha_connected;
    bool mqtt_connected;
    bool ota_url_set;
    oled_va_state_t va_state;
    oled_tts_state_t tts_state;
    oled_ota_state_t ota_state;
    oled_music_state_t music_state;
    int music_track;
    int music_total;
    char last_event[12];
    char response_preview[12];
    bool dirty;
} oled_status_snapshot_t;

static SemaphoreHandle_t status_mutex = NULL;
static oled_status_snapshot_t status_snapshot;
static i2c_master_dev_handle_t oled_dev = NULL;
static uint8_t oled_addr = 0;
static uint8_t framebuffer[OLED_FB_SIZE];

static const uint8_t font8x8_basic[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 32
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // 33
    {0x36,0x36,0x24,0x00,0x00,0x00,0x00,0x00}, // 34
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // 35
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // 36
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // 37
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // 38
    {0x06,0x06,0x04,0x00,0x00,0x00,0x00,0x00}, // 39
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // 40
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // 41
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // 42
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // 43
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // 44
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // 45
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // 46
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // 47
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // 48
    {0x0C,0x0E,0x0F,0x0C,0x0C,0x0C,0x3F,0x00}, // 49
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 50
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 51
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 52
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 53
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 54
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 55
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 56
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 57
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // 58
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // 59
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // 60
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // 61
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // 62
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // 63
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // 64
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // 65
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // 66
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // 67
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // 68
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // 69
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // 70
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // 71
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // 72
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 73
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // 74
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // 75
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // 76
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // 77
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // 78
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // 79
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // 80
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // 81
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // 82
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // 83
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 84
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // 85
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 86
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 87
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // 88
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // 89
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // 90
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // 91
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // 92
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // 93
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // 94
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // 95
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // 96
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // 97
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // 98
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // 99
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, // 100
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, // 101
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, // 102
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // 103
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // 104
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // 105
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // 106
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // 107
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 108
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // 109
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // 110
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // 111
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // 112
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // 113
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // 114
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // 115
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // 116
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // 117
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 118
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // 119
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // 120
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // 121
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // 122
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // 123
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // 124
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // 125
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // 126
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}  // 127
};

static void status_lock(void) {
    if (status_mutex) {
        xSemaphoreTake(status_mutex, portMAX_DELAY);
    }
}

static void status_unlock(void) {
    if (status_mutex) {
        xSemaphoreGive(status_mutex);
    }
}

static void status_mark_dirty(void) {
    status_snapshot.dirty = true;
}

static char sanitize_ascii(char c) {
    if (c < 32 || c > 126) {
        return '?';
    }
    return c;
}

static void fb_clear(void) {
    memset(framebuffer, 0, sizeof(framebuffer));
}

static void fb_set_pixel(int x, int y, bool on) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    int index = x + (y / 8) * OLED_WIDTH;
    uint8_t mask = 1 << (y & 7);
    if (on) {
        framebuffer[index] |= mask;
    } else {
        framebuffer[index] &= ~mask;
    }
}

static void fb_draw_char(int x, int y, char c) {
    char safe = sanitize_ascii(c);
    if (safe < 32 || safe > 127) {
        safe = '?';
    }
    const uint8_t *glyph = font8x8_basic[safe - 32];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            bool on = (bits >> col) & 0x01;
            fb_set_pixel(x + col, y + row, on);
        }
    }
}

static void fb_draw_text(int line, int col, const char *text) {
    if (!text || line < 0 || line >= 8 || col < 0 || col >= 16) {
        return;
    }
    int x = col * 8;
    int y = line * 8;
    for (int i = 0; text[i] != '\0' && col + i < 16; i++) {
        fb_draw_char(x + i * 8, y, text[i]);
    }
}

static void format_line(char *out, size_t out_len, const char *text) {
    if (!out || out_len < 17) {
        return;
    }
    char tmp[17];
    if (text) {
        strncpy(tmp, text, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
    } else {
        tmp[0] = '\0';
    }
    memset(out, ' ', 16);
    out[16] = '\0';
    if (tmp[0] == '\0') {
        return;
    }
    size_t len = strlen(tmp);
    if (len > 16) {
        len = 16;
    }
    memcpy(out, tmp, len);
}

static esp_err_t oled_write(const uint8_t *data, size_t len) {
    if (!oled_dev || !data || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit(oled_dev, data, len, OLED_I2C_TIMEOUT_MS);
}

static esp_err_t oled_write_cmds(const uint8_t *cmds, size_t len) {
    if (!cmds || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[16];
    if (len > (sizeof(buf) - 1)) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = 0x00;
    memcpy(&buf[1], cmds, len);
    return oled_write(buf, len + 1);
}

static esp_err_t oled_flush(void) {
    for (uint8_t page = 0; page < OLED_PAGES; page++) {
        uint8_t cmds[] = {0xB0 | page, 0x00, 0x10};
        if (oled_write_cmds(cmds, sizeof(cmds)) != ESP_OK) {
            return ESP_FAIL;
        }
        uint8_t buf[OLED_WIDTH + 1];
        buf[0] = 0x40;
        memcpy(&buf[1], &framebuffer[page * OLED_WIDTH], OLED_WIDTH);
        if (oled_write(buf, sizeof(buf)) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t oled_init_device(uint8_t addr) {
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        return ESP_FAIL;
    }

    i2c_device_config_t dev_cfg = {
        .device_address = addr,
        .scl_speed_hz = OLED_I2C_SPEED_HZ,
    };

    if (i2c_master_bus_add_device(bus, &dev_cfg, &oled_dev) != ESP_OK) {
        return ESP_FAIL;
    }

    uint8_t probe_cmds[] = {0xAE};
    if (oled_write_cmds(probe_cmds, sizeof(probe_cmds)) != ESP_OK) {
        i2c_master_bus_rm_device(oled_dev);
        oled_dev = NULL;
        return ESP_FAIL;
    }

    uint8_t init_cmds[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF
    };

    if (oled_write_cmds(init_cmds, sizeof(init_cmds)) != ESP_OK) {
        i2c_master_bus_rm_device(oled_dev);
        oled_dev = NULL;
        return ESP_FAIL;
    }

    oled_addr = addr;
    return ESP_OK;
}

static void render_page_overview(const oled_status_snapshot_t *snap) {
    char line[17];

    const char *mode = snap->safe_mode ? "SAFE" : "NORM";
    const char *va = "IDLE";
    switch (snap->va_state) {
        case OLED_VA_LISTENING: va = "LSTN"; break;
        case OLED_VA_PROCESSING: va = "PROC"; break;
        case OLED_VA_SPEAKING: va = "SPK"; break;
        case OLED_VA_ERROR: va = "ERR"; break;
        case OLED_VA_IDLE:
        default: va = "IDLE"; break;
    }
    snprintf(line, sizeof(line), "M:%s VA:%s", mode, va);
    format_line(line, sizeof(line), line);
    fb_draw_text(0, 0, line);

    const char *ha = snap->ha_connected ? "OK" : "NO";
    const char *mq = snap->mqtt_connected ? "OK" : "NO";
    const char *net = "O";
    network_type_t net_type = network_manager_get_active_type();
    if (net_type == NETWORK_TYPE_ETHERNET) net = "E";
    else if (net_type == NETWORK_TYPE_WIFI) net = "W";
    snprintf(line, sizeof(line), "HA:%s MQ:%s N:%s", ha, mq, net);
    format_line(line, sizeof(line), line);
    fb_draw_text(1, 0, line);

    char ip_str[16];
    if (network_manager_get_ip(ip_str) != ESP_OK) {
        strncpy(ip_str, "0.0.0.0", sizeof(ip_str));
        ip_str[sizeof(ip_str) - 1] = '\0';
    }
    snprintf(line, sizeof(line), "IP:%s", ip_str);
    format_line(line, sizeof(line), line);
    fb_draw_text(2, 0, line);

    int vol = bsp_extra_codec_volume_get();
    int led = led_status_get_brightness();
    snprintf(line, sizeof(line), "VOL:%d%% LED:%d%%", vol, led);
    format_line(line, sizeof(line), line);
    fb_draw_text(3, 0, line);

    const char *ota = "IDLE";
    switch (snap->ota_state) {
        case OLED_OTA_RUNNING: ota = "RUN"; break;
        case OLED_OTA_OK: ota = "OK"; break;
        case OLED_OTA_ERROR: ota = "ERR"; break;
        case OLED_OTA_IDLE:
        default: ota = "IDLE"; break;
    }
    const char *tts = "ID";
    switch (snap->tts_state) {
        case OLED_TTS_DOWNLOADING: tts = "DL"; break;
        case OLED_TTS_PLAYING: tts = "PLY"; break;
        case OLED_TTS_ERROR: tts = "ERR"; break;
        case OLED_TTS_IDLE:
        default: tts = "ID"; break;
    }
    snprintf(line, sizeof(line), "OTA:%s TTS:%s", ota, tts);
    format_line(line, sizeof(line), line);
    fb_draw_text(4, 0, line);

    uint64_t uptime = (uint64_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t hrs = (uint32_t)(uptime / 3600ULL);
    uint32_t mins = (uint32_t)((uptime % 3600ULL) / 60ULL);
    uint32_t secs = (uint32_t)(uptime % 60ULL);
    snprintf(line, sizeof(line), "UP:%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32, hrs, mins, secs);
    format_line(line, sizeof(line), line);
    fb_draw_text(5, 0, line);

    size_t heap_kb = esp_get_free_heap_size() / 1024;
    size_t psram_kb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    if (psram_kb >= 1024) {
        snprintf(line, sizeof(line), "HP:%zuK PS:%zuM", heap_kb, psram_kb / 1024);
    } else {
        snprintf(line, sizeof(line), "HP:%zuK PS:%zuK", heap_kb, psram_kb);
    }
    format_line(line, sizeof(line), line);
    fb_draw_text(6, 0, line);

    int boot_count = sys_diag_get_boot_count();
    const char *rr = "OTH";
    const char *full = sys_diag_get_reset_reason();
    if (full) {
        if (strstr(full, "WDT")) rr = "WDT";
        else if (strstr(full, "Crash") || strstr(full, "Panic")) rr = "PAN";
        else if (strstr(full, "Power")) rr = "PWR";
        else if (strstr(full, "Software")) rr = "SW";
        else if (strstr(full, "Brownout")) rr = "BRN";
    }
    snprintf(line, sizeof(line), "BOOT:%d R:%s", boot_count, rr);
    format_line(line, sizeof(line), line);
    fb_draw_text(7, 0, line);
}

static void render_page_network(const oled_status_snapshot_t *snap) {
    (void)snap;
    char line[17];

    network_type_t net_type = network_manager_get_active_type();
    const char *net = "OFF";
    if (net_type == NETWORK_TYPE_ETHERNET) net = "ETH";
    else if (net_type == NETWORK_TYPE_WIFI) net = "WIFI";
    const char *link = (net_type == NETWORK_TYPE_NONE) ? "DOWN" : "UP";
    snprintf(line, sizeof(line), "NET:%s LINK:%s", net, link);
    format_line(line, sizeof(line), line);
    fb_draw_text(0, 0, line);

    char ip_str[16];
    if (network_manager_get_ip(ip_str) != ESP_OK) {
        strncpy(ip_str, "0.0.0.0", sizeof(ip_str));
        ip_str[sizeof(ip_str) - 1] = '\0';
    }
    snprintf(line, sizeof(line), "IP:%s", ip_str);
    format_line(line, sizeof(line), line);
    fb_draw_text(1, 0, line);

    esp_netif_ip_info_t ip_info;
    if (network_manager_get_ip_info(&ip_info) == ESP_OK) {
        char gw_str[16];
        snprintf(gw_str, sizeof(gw_str), IPSTR, IP2STR(&ip_info.gw));
        snprintf(line, sizeof(line), "GW:%s", gw_str);
    } else {
        snprintf(line, sizeof(line), "GW:0.0.0.0");
    }
    format_line(line, sizeof(line), line);
    fb_draw_text(2, 0, line);

    esp_netif_dns_info_t dns_info;
    if (network_manager_get_dns_info(&dns_info) == ESP_OK) {
        char dns_str[16];
        snprintf(dns_str, sizeof(dns_str), IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        snprintf(line, sizeof(line), "DNS:%s", dns_str);
    } else {
        snprintf(line, sizeof(line), "DNS:0.0.0.0");
    }
    format_line(line, sizeof(line), line);
    fb_draw_text(3, 0, line);

    if (net_type == NETWORK_TYPE_WIFI) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            snprintf(line, sizeof(line), "RSSI:%ddBm", ap_info.rssi);
        } else {
            snprintf(line, sizeof(line), "RSSI:--");
        }
    } else {
        snprintf(line, sizeof(line), "RSSI:--");
    }
    format_line(line, sizeof(line), line);
    fb_draw_text(4, 0, line);

    snprintf(line, sizeof(line), "MDNS:-- WEB:ON");
    format_line(line, sizeof(line), line);
    fb_draw_text(5, 0, line);

    snprintf(line, sizeof(line), "LASTCHG:--");
    format_line(line, sizeof(line), line);
    fb_draw_text(6, 0, line);

    snprintf(line, sizeof(line), "ERR:-");
    format_line(line, sizeof(line), line);
    fb_draw_text(7, 0, line);
}

static void render_page_pipeline(const oled_status_snapshot_t *snap) {
    char line[17];

    const char *ha = snap->ha_connected ? "OK" : "NO";
    const char *aud = ha_client_is_audio_ready() ? "OK" : "NO";
    const char *mq = snap->mqtt_connected ? "OK" : "NO";
    snprintf(line, sizeof(line), "HA:%s A:%s MQ:%s", ha, aud, mq);
    format_line(line, sizeof(line), line);
    fb_draw_text(0, 0, line);

    int bin = ha_client_get_stt_binary_handler_id();
    if (bin >= 0) {
        snprintf(line, sizeof(line), "WS:OK BIN:%02X", bin & 0xFF);
    } else {
        snprintf(line, sizeof(line), "WS:%s BIN:--", snap->ha_connected ? "OK" : "NO");
    }
    format_line(line, sizeof(line), line);
    fb_draw_text(1, 0, line);

    const char *va = "IDLE";
    switch (snap->va_state) {
        case OLED_VA_LISTENING: va = "LSTN"; break;
        case OLED_VA_PROCESSING: va = "PROC"; break;
        case OLED_VA_SPEAKING: va = "SPK"; break;
        case OLED_VA_ERROR: va = "ERR"; break;
        case OLED_VA_IDLE:
        default: va = "IDLE"; break;
    }
    const char *wwd = va_control_get_wwd_running() ? "ON" : "OFF";
    snprintf(line, sizeof(line), "VA:%s WWD:%s", va, wwd);
    format_line(line, sizeof(line), line);
    fb_draw_text(2, 0, line);

    snprintf(line, sizeof(line), "STG:STT STRM:%s", ha_client_is_audio_ready() ? "ON" : "OFF");
    format_line(line, sizeof(line), line);
    fb_draw_text(3, 0, line);

    const char *tts = "ID";
    switch (snap->tts_state) {
        case OLED_TTS_DOWNLOADING: tts = "DL"; break;
        case OLED_TTS_PLAYING: tts = "PLY"; break;
        case OLED_TTS_ERROR: tts = "ERR"; break;
        case OLED_TTS_IDLE:
        default: tts = "ID"; break;
    }
    snprintf(line, sizeof(line), "TTS:%s URL:%s", tts, snap->ota_url_set ? "OK" : "--");
    format_line(line, sizeof(line), line);
    fb_draw_text(4, 0, line);

    snprintf(line, sizeof(line), "RESP:%s", snap->response_preview[0] ? snap->response_preview : "-");
    format_line(line, sizeof(line), line);
    fb_draw_text(5, 0, line);

    snprintf(line, sizeof(line), "EV:%s", snap->last_event[0] ? snap->last_event : "-");
    format_line(line, sizeof(line), line);
    fb_draw_text(6, 0, line);

    snprintf(line, sizeof(line), "ERR:-");
    format_line(line, sizeof(line), line);
    fb_draw_text(7, 0, line);
}

static void render_page_audio(const oled_status_snapshot_t *snap) {
    char line[17];

    int vol = bsp_extra_codec_volume_get();
    int led = led_status_get_brightness();
    snprintf(line, sizeof(line), "VOL:%d%% LED:%d%%", vol, led);
    format_line(line, sizeof(line), line);
    fb_draw_text(0, 0, line);

    const char *mus = "OFF";
    switch (snap->music_state) {
        case OLED_MUSIC_PLAYING: mus = "PLY"; break;
        case OLED_MUSIC_PAUSED: mus = "PAU"; break;
        case OLED_MUSIC_OFF:
        default: mus = "OFF"; break;
    }
    if (snap->music_total > 0 && snap->music_track >= 0) {
        snprintf(line, sizeof(line), "MUS:%s TR:%02d/%02d", mus, snap->music_track + 1, snap->music_total);
    } else {
        snprintf(line, sizeof(line), "MUS:%s TR:--/--", mus);
    }
    format_line(line, sizeof(line), line);
    fb_draw_text(1, 0, line);

    const char *tts = "ID";
    switch (snap->tts_state) {
        case OLED_TTS_DOWNLOADING: tts = "DL"; break;
        case OLED_TTS_PLAYING: tts = "PLY"; break;
        case OLED_TTS_ERROR: tts = "ERR"; break;
        case OLED_TTS_IDLE:
        default: tts = "ID"; break;
    }
    snprintf(line, sizeof(line), "TTS:%s BEEP:ON", tts);
    format_line(line, sizeof(line), line);
    fb_draw_text(2, 0, line);

    float wwd = va_control_get_wwd_threshold();
    snprintf(line, sizeof(line), "WWD:%.2f VAD:%s", (double)wwd, VAD_ENABLED ? "ON" : "OFF");
    format_line(line, sizeof(line), line);
    fb_draw_text(3, 0, line);

    snprintf(line, sizeof(line), "AEC:%s AGC:%s", ENABLE_AEC ? "ON" : "OFF",
             va_control_get_agc_enabled() ? "ON" : "OFF");
    format_line(line, sizeof(line), line);
    fb_draw_text(4, 0, line);

    snprintf(line, sizeof(line), "AFE:%s SR:%s", "ON", "16K");
    format_line(line, sizeof(line), line);
    fb_draw_text(5, 0, line);

    snprintf(line, sizeof(line), "I2C:OK OLED:%02X", oled_addr);
    format_line(line, sizeof(line), line);
    fb_draw_text(6, 0, line);

    snprintf(line, sizeof(line), "SD:%s", bsp_sdcard ? "OK" : "NO");
    format_line(line, sizeof(line), line);
    fb_draw_text(7, 0, line);
}

static void render_page(uint8_t page, const oled_status_snapshot_t *snap) {
    fb_clear();
    switch (page) {
        case 0:
            render_page_overview(snap);
            break;
        case 1:
            render_page_network(snap);
            break;
        case 2:
            render_page_pipeline(snap);
            break;
        case 3:
        default:
            render_page_audio(snap);
            break;
    }
}

static void oled_task(void *arg) {
    (void)arg;
    uint8_t page = 0;
    int64_t last_page_switch = esp_timer_get_time();
    int64_t last_refresh = 0;
    size_t last_heap_kb = 0;
    int last_rssi = 0;

    while (1) {
        int64_t now = esp_timer_get_time();
        bool refresh = false;

        status_lock();
        if (status_snapshot.dirty) {
            refresh = true;
            status_snapshot.dirty = false;
        }
        status_unlock();

        size_t heap_kb = esp_get_free_heap_size() / 1024;
        if (heap_kb > last_heap_kb + 10 || heap_kb + 10 < last_heap_kb) {
            refresh = true;
            last_heap_kb = heap_kb;
        }

        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            if (ap_info.rssi >= last_rssi + 3 || ap_info.rssi <= last_rssi - 3) {
                refresh = true;
                last_rssi = ap_info.rssi;
            }
        }

        if (now - last_page_switch >= (int64_t)OLED_PAGE_ROTATE_MS * 1000LL) {
            page = (page + 1) % 4;
            last_page_switch = now;
            refresh = true;
        }

        if (refresh && now - last_refresh >= (int64_t)OLED_REFRESH_MIN_MS * 1000LL) {
            oled_status_snapshot_t snap;
            status_lock();
            snap = status_snapshot;
            status_unlock();
            render_page(page, &snap);
            if (oled_flush() != ESP_OK) {
                ESP_LOGW(TAG, "OLED flush failed");
            }
            last_refresh = now;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void status_init_defaults(void) {
    memset(&status_snapshot, 0, sizeof(status_snapshot));
    status_snapshot.va_state = OLED_VA_IDLE;
    status_snapshot.tts_state = OLED_TTS_IDLE;
    status_snapshot.ota_state = OLED_OTA_IDLE;
    status_snapshot.music_state = OLED_MUSIC_OFF;
    status_snapshot.music_track = -1;
    status_snapshot.music_total = 0;
}

esp_err_t oled_status_init(void) {
    status_mutex = xSemaphoreCreateMutex();
    status_init_defaults();

    status_snapshot.safe_mode = sys_diag_is_safe_mode();
    status_snapshot.ha_connected = ha_client_is_connected();
    status_snapshot.mqtt_connected = mqtt_ha_is_connected();
    status_snapshot.ota_url_set = false;
    status_snapshot.enabled = false;

    if (bsp_i2c_init() != ESP_OK) {
        ESP_LOGW(TAG, "I2C init failed, OLED disabled");
        return ESP_FAIL;
    }

    if (oled_init_device(OLED_ADDR_PRIMARY) != ESP_OK &&
        oled_init_device(OLED_ADDR_FALLBACK) != ESP_OK) {
        ESP_LOGW(TAG, "OLED not detected, running without display");
        return ESP_FAIL;
    }

    status_snapshot.enabled = true;
    status_mark_dirty();

    fb_clear();
    (void)oled_flush();

    xTaskCreate(oled_task, "oled_task", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "OLED task started (addr 0x%02X)", oled_addr);
    return ESP_OK;
}

void oled_status_set_safe_mode(bool enabled) {
    status_lock();
    if (status_snapshot.safe_mode != enabled) {
        status_snapshot.safe_mode = enabled;
        status_mark_dirty();
    }
    status_unlock();
}

void oled_status_set_ha_connected(bool connected) {
    status_lock();
    if (status_snapshot.ha_connected != connected) {
        status_snapshot.ha_connected = connected;
        status_mark_dirty();
    }
    status_unlock();
}

void oled_status_set_mqtt_connected(bool connected) {
    status_lock();
    if (status_snapshot.mqtt_connected != connected) {
        status_snapshot.mqtt_connected = connected;
        status_mark_dirty();
    }
    status_unlock();
}

void oled_status_set_va_state(oled_va_state_t state) {
    status_lock();
    if (status_snapshot.va_state != state) {
        status_snapshot.va_state = state;
        status_mark_dirty();
    }
    status_unlock();
}

void oled_status_set_tts_state(oled_tts_state_t state) {
    status_lock();
    if (status_snapshot.tts_state != state) {
        status_snapshot.tts_state = state;
        status_mark_dirty();
    }
    status_unlock();
}

void oled_status_set_ota_state(oled_ota_state_t state) {
    status_lock();
    if (status_snapshot.ota_state != state) {
        status_snapshot.ota_state = state;
        status_mark_dirty();
    }
    status_unlock();
}

void oled_status_set_music_state(oled_music_state_t state, int current_track, int total_tracks) {
    status_lock();
    if (status_snapshot.music_state != state ||
        status_snapshot.music_track != current_track ||
        status_snapshot.music_total != total_tracks) {
        status_snapshot.music_state = state;
        status_snapshot.music_track = current_track;
        status_snapshot.music_total = total_tracks;
        status_mark_dirty();
    }
    status_unlock();
}

void oled_status_set_last_event(const char *code) {
    if (!code) {
        return;
    }
    status_lock();
    if (strncmp(status_snapshot.last_event, code, sizeof(status_snapshot.last_event)) != 0) {
        snprintf(status_snapshot.last_event, sizeof(status_snapshot.last_event), "%s", code);
        status_mark_dirty();
    }
    status_unlock();
}

void oled_status_set_response_preview(const char *text) {
    status_lock();
    if (!text || text[0] == '\0') {
        status_snapshot.response_preview[0] = '\0';
        status_mark_dirty();
        status_unlock();
        return;
    }
    char buf[12];
    size_t len = strlen(text);
    if (len > 11) {
        len = 11;
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = sanitize_ascii(text[i]);
    }
    buf[len] = '\0';
    if (strncmp(status_snapshot.response_preview, buf, sizeof(status_snapshot.response_preview)) != 0) {
        snprintf(status_snapshot.response_preview, sizeof(status_snapshot.response_preview), "%s", buf);
        status_mark_dirty();
    }
    status_unlock();
}

void oled_status_set_ota_url_present(bool present) {
    status_lock();
    if (status_snapshot.ota_url_set != present) {
        status_snapshot.ota_url_set = present;
        status_mark_dirty();
    }
    status_unlock();
}
