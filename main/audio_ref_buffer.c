#include "audio_ref_buffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "audio_ref";
static RingbufHandle_t ref_rb = NULL;

esp_err_t audio_ref_buffer_init(size_t size) {
    if (ref_rb) return ESP_OK;
    
    // Create ring buffer (No-Split for simpler byte stream, or Allow-Split)
    // We use RINGBUF_TYPE_BYTEBUF for simple stream
    ref_rb = xRingbufferCreate(size, RINGBUF_TYPE_BYTEBUF);
    if (!ref_rb) {
        ESP_LOGE(TAG, "Failed to create reference ring buffer");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Reference buffer initialized (%d bytes)", size);
    return ESP_OK;
}

void audio_ref_buffer_write(const void *data, size_t len) {
    if (!ref_rb || !data || len == 0) return;
    
    // Send data to ring buffer. If full, we drop old data? 
    // Ideally we block briefly or overwrite. Ringbuf doesn't support overwrite easily.
    // We use a short timeout. If player is faster than capture, we drop to avoid blocking player.
    if (xRingbufferSend(ref_rb, data, len, 0) != pdTRUE) {
        // Buffer full - consumer (AFE) is too slow or stopped.
        // We could try to reset, but that causes sync issues.
        // For AEC, it's better to have gaps than delay? 
        // Actually, delay kills AEC. If full, it means desync.
        // Just drop for now.
        // ESP_LOGW(TAG, "Ref buffer full, dropped %d bytes", len);
    }
}

size_t audio_ref_buffer_read(void *dest, size_t len) {
    if (!ref_rb || !dest || len == 0) {
        memset(dest, 0, len);
        return 0;
    }

    size_t read_bytes = 0;
    void *data = xRingbufferReceiveUpTo(ref_rb, &read_bytes, 0, len);
    
    if (data) {
        if (read_bytes > len) read_bytes = len; // Should be handled by UpTo
        memcpy(dest, data, read_bytes);
        vRingbufferReturnItem(ref_rb, data);
        
        // Fill rest with silence if not enough data
        if (read_bytes < len) {
            memset((uint8_t*)dest + read_bytes, 0, len - read_bytes);
        }
        return read_bytes;
    } else {
        // No reference data (Silence)
        memset(dest, 0, len);
        return 0;
    }
}
