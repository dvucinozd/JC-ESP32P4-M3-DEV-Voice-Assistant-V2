# MQTT Home Assistant Integration

## Overview

ESP32-P4 Voice Assistant supports full Home Assistant integration via **MQTT Discovery protocol**. The device automatically appears in Home Assistant with sensors, switches, and controls.

## Features

### Auto-Discovery
- Device automatically discovered by Home Assistant
- No manual YAML configuration needed
- All entities grouped under single device

### Real-Time Monitoring
- WiFi signal strength (RSSI)
- Free memory (heap)
- System uptime
- Wake Word Detection status

### Remote Control
- Enable/Disable Wake Word Detection
- Restart device
- Test TTS functionality

## Configuration

### MQTT Broker Setup

Ensure MQTT broker (Mosquitto) is running on your Home Assistant instance:

```bash
# Check if MQTT is running
docker ps | grep mosquitto

# Install MQTT broker if not present (HA Supervisor)
# Go to: Settings → Add-ons → Mosquitto broker → Install
```

### Device Configuration

Edit `main/config.h`:

```c
#define MQTT_BROKER_URI "mqtt://homeassistant.local:1883"
#define MQTT_USERNAME NULL  // Set if authentication enabled
#define MQTT_PASSWORD NULL
#define MQTT_CLIENT_ID "esp32p4_voice_assistant"
```

## Home Assistant Entities

Once connected, the following entities will appear:

### Sensors (Read-Only)

| Entity ID | Name | Unit | Description |
|-----------|------|------|-------------|
| `sensor.esp32_p4_voice_assistant_wifi_signal` | WiFi Signal | dBm | WiFi signal strength |
| `sensor.esp32_p4_voice_assistant_free_memory` | Free Memory | KB | Available heap memory |
| `sensor.esp32_p4_voice_assistant_uptime` | Uptime | s | System uptime in seconds |

### Switches

| Entity ID | Name | Description |
|-----------|------|-------------|
| `switch.esp32_p4_voice_assistant_wake_word_detection` | Wake Word Detection | Enable/disable wake word detection |

### Buttons

| Entity ID | Name | Description |
|-----------|------|-------------|
| `button.esp32_p4_voice_assistant_restart_device` | Restart Device | Restart ESP32-P4 |
| `button.esp32_p4_voice_assistant_test_tts` | Test TTS | Test text-to-speech |

## MQTT Topics

### Discovery Topics
```
homeassistant/sensor/esp32p4_voice_assistant/<entity>/config
homeassistant/switch/esp32p4_voice_assistant/<entity>/config
homeassistant/button/esp32p4_voice_assistant/<entity>/config
```

### State Topics
```
esp32p4/wifi_rssi/state
esp32p4/free_memory/state
esp32p4/uptime/state
esp32p4/wwd_enabled/state
```

### Command Topics
```
esp32p4/wwd_enabled/set
esp32p4/restart/set
esp32p4/test_tts/set
```

## Usage Examples

### Lovelace Dashboard Card

Add this to your Lovelace dashboard:

```yaml
type: entities
title: ESP32-P4 Voice Assistant
entities:
  - entity: sensor.esp32_p4_voice_assistant_wifi_signal
    name: WiFi Signal
  - entity: sensor.esp32_p4_voice_assistant_free_memory
    name: Free Memory
  - entity: sensor.esp32_p4_voice_assistant_uptime
    name: Uptime
  - entity: switch.esp32_p4_voice_assistant_wake_word_detection
    name: Wake Word Detection
  - entity: button.esp32_p4_voice_assistant_restart_device
    name: Restart
  - entity: button.esp32_p4_voice_assistant_test_tts
    name: Test TTS
```

### Automation Example

Automatically disable wake word detection at night:

```yaml
automation:
  - alias: "Disable Voice Assistant at Night"
    trigger:
      - platform: time
        at: "23:00:00"
    action:
      - service: switch.turn_off
        target:
          entity_id: switch.esp32_p4_voice_assistant_wake_word_detection

  - alias: "Enable Voice Assistant in Morning"
    trigger:
      - platform: time
        at: "07:00:00"
    action:
      - service: switch.turn_on
        target:
          entity_id: switch.esp32_p4_voice_assistant_wake_word_detection
```

### Low Memory Alert

```yaml
automation:
  - alias: "ESP32-P4 Low Memory Alert"
    trigger:
      - platform: numeric_state
        entity_id: sensor.esp32_p4_voice_assistant_free_memory
        below: 100  # KB
    action:
      - service: notify.mobile_app
        data:
          message: "ESP32-P4 Voice Assistant is low on memory!"
```

## Debugging

### View MQTT Messages

Monitor MQTT traffic using MQTT Explorer or command line:

```bash
# Subscribe to all ESP32-P4 topics
mosquitto_sub -h homeassistant.local -t "esp32p4/#" -v

# Subscribe to discovery topics
mosquitto_sub -h homeassistant.local -t "homeassistant/+/esp32p4_voice_assistant/#" -v
```

### Serial Monitor

Check ESP32-P4 serial output for MQTT logs:

```
I (12345) mqtt_ha: MQTT connected to Home Assistant
I (12350) mqtt_ha: Publishing discovery: homeassistant/sensor/esp32p4_voice_assistant/wifi_rssi/config
I (12360) mqtt_ha: Subscribed to command topic: esp32p4/wwd_enabled/set
```

### Home Assistant MQTT Integration

Ensure MQTT integration is configured in Home Assistant:

1. Settings → Devices & Services
2. Find "MQTT" integration
3. Should show "1 device" (ESP32-P4 Voice Assistant)

## Troubleshooting

### Device Not Appearing

1. Check MQTT broker is running: `docker ps | grep mosquitto`
2. Verify ESP32-P4 is connected to WiFi
3. Check serial monitor for MQTT connection errors
4. Restart Home Assistant: Settings → System → Restart

### Entities Not Updating

1. Check MQTT status update task is running (serial monitor)
2. Verify network connectivity
3. Check MQTT broker logs: `docker logs mosquitto`

### Commands Not Working

1. Verify command topics are subscribed (serial monitor)
2. Test with MQTT Explorer or mosquitto_pub
3. Check callback functions are registered

## Technical Details

### Implementation Files

- `main/mqtt_ha.h` - MQTT HA API header
- `main/mqtt_ha.c` - MQTT client implementation
- `main/main.c` - Main integration and callbacks

### Device Information

```json
{
  "identifiers": ["esp32p4_voice_assistant"],
  "name": "ESP32-P4 Voice Assistant",
  "model": "JC-ESP32P4-M3-DEV",
  "manufacturer": "Guition",
  "sw_version": "1.0.0"
}
```

### Update Interval

Status sensors update every **10 seconds** via background task.

## Future Enhancements

Potential additions:

- [ ] Number entity for VAD threshold adjustment
- [ ] Number entity for microphone gain control
- [ ] Select entity for wake word sensitivity presets
- [ ] Binary sensor for audio playback status
- [ ] Diagnostic sensors (WiFi reconnects, errors)
- [ ] Configuration entities (sample rate, bit depth)

## References

- [Home Assistant MQTT Discovery](https://www.home-assistant.io/integrations/mqtt/#mqtt-discovery)
- [ESP-MQTT Component](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mqtt.html)
- [MQTT Protocol](https://mqtt.org/)
