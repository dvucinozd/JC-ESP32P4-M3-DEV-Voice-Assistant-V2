# TODO: OLED status display (SSD1306 128x64, I2C shared with ES8311)

Goal: Add a small **text-only** OLED to show service/status info with **rotating pages**, and align LED behavior so the device is not “green/ready” when HA is unavailable.

## Hardware / wiring

- OLED: 0.96" `SSD1306` 128x64, 4-pin I2C module
- Wiring:
  - `VCC -> 3V3`
  - `GND -> GND`
  - `SDA -> ES I2C_SDA`
  - `SCL -> ES I2C_SCL`
- I2C address:
  - Try `0x3C` first, fallback to `0x3D` if no ACK.
- Ensure I2C pull-ups exist (module usually has them). If unstable, verify/adjust pull-up strength.

## I2C bus sharing (codec + OLED)

- Add a single “I2C bus owner” abstraction:
  - Initialize I2C master once (works in normal + safe mode).
  - Provide a mutex/lock for I2C transactions to avoid concurrent writes (ES8311 + SSD1306).
- OLED refresh must be low priority and non-blocking:
  - Use short timeouts on I2C writes.
  - Cap refresh rate (e.g. max 5 Hz) and coalesce frequent updates.
- If OLED is missing/not connected, the system must continue normally:
  - On init, probe `0x3C` then `0x3D` and, if no ACK, mark OLED as disabled.
  - All OLED update calls should be no-ops when disabled; no retries that block boot.

## Display architecture

### Status snapshot model

- Create `device_status_t` snapshot in RAM (thread-safe copy/update):
  - `mode`: `NORM` / `SAFE`
  - `net`: `OFF` / `ETH` / `WIFI`
  - `ip`: `a.b.c.d`
  - `rssi`: `-NN dBm` (wifi only)
  - `ha`: `OK` / `NO` (WebSocket connected + authenticated)
  - `mqtt`: `OK` / `NO`
  - `va`: `IDLE` / `LSTN` / `PROC` / `SPK` / `ERR`
  - `tts`: `ID` / `DL` / `PLY` / `ERR`
  - `ota`: `IDLE` / `RUN` / `OK` / `ERR`
  - `music`: `OFF` / `PLY` / `PAU`
  - `vol_pct`: `0..100`
  - `led_pct`: `0..100`
  - `uptime`: `HH:MM:SS`
  - `heap_kb`, `psram_kb`
  - `boot_count`, `reset_reason_short` (`PWR/SW/WDT/PAN/BRN/OTH`)
  - `wwd_threshold` (`0.50..0.95`)
  - `stt_bin_id` (`--` or `00..FF`)
  - `last_event` (see EV codes below)
- Modules update snapshot via minimal setters (no direct OLED access):
  - `network_manager` (type/ip up/down)
  - `wifi_manager` (RSSI when wifi)
  - `ha_client` (ws up/down, auth ok/bad, stt bin id)
  - `mqtt_ha` (mqtt up/down)
  - `voice_pipeline` (IDLE/LSTN/PROC/SPK)
  - `tts_player` (DL/PLY/DONE/ERR)
  - `ota_update` (RUN/OK/ERR)
  - `sys_diag` (SAFE + reset reason + boot count)
  - `led_status` (brightness)
  - `bsp_extra_codec` (volume)
- Snapshot sync (pick one and be consistent):
  - Mutex-protected copy/update (simple): setters take a mutex, update fields, set a dirty flag.
  - Or lock-free double buffer: writers update a staging struct + increment version; reader copies when version changes.
- Keep OLED module isolated: no circular includes; expose a tiny `oled_status_set_*()` API in `oled_status.h`.

### SSD1306 text-only driver

- Implement minimal SSD1306 I2C driver:
  - `ssd1306_init(addr, 128, 64)`
  - `ssd1306_clear()`
  - `ssd1306_flush(framebuffer_1024B)`
  - `ssd1306_draw_text(line, col, ascii)` (8x8 font; 16x8 grid)
- ASCII-only rendering; replace non-ASCII with `?`.

### OLED task

- One FreeRTOS task `oled_task`:
  - Rotates pages every `2500 ms` (configurable).
  - “Dirty refresh” when any critical snapshot field changes.
  - Rate limit refresh (e.g. 200 ms minimum between flushes).
  - If OLED init fails (no ACK on `0x3C/0x3D`), disable task and log once.
- I2C timing (suggested):
  - Per-transaction timeout: 20-30 ms.
  - Overall refresh budget: <= 50 ms (skip frame if exceeded).
- Dirty refresh thresholds:
  - Numeric deltas (RSSI change >= 3 dB, heap change >= 10 KB).
  - State changes always trigger (HA/MQTT up/down, VA state, OTA state).

## Page layout (16x8, ASCII only)

Notes:
- Each line max 16 chars.
- Fixed tokens to avoid wrapping.
- Truncate response preview to 11 chars.

### Page 1/4: OVERVIEW
```
M:NORM VA:IDLE
HA:OK MQ:OK N:E
IP:192.168.x.x
VOL:60% LED:20%
OTA:IDLE TTS:ID
UP:12:34:56
HP:123K PS:2M
BOOT:2 R:WDT
```

### Page 2/4: NETWORK
```
NET:ETH LINK:UP
IP:192.168.x.x
GW:192.168.x.x
DNS:192.168.x.x
RSSI:-55dBm
MDNS:OK WEB:ON
LASTCHG:00012s
ERR:-
```

### Page 3/4: HA/PIPELINE
```
HA:OK A:OK MQ:OK
WS:OK BIN:06
VA:LSTN WWD:ON
STG:STT STRM:ON
TTS:DL  URL:OK
RESP:Hello world
EV:intent-end
ERR:-
```

### Page 4/4: AUDIO/DIAG
```
VOL:60% LED:20%
MUS:PLY TR:03/12
TTS:PLY BEEP:ON
WWD:0.85 VAD:ON
AEC:ON AGC:ON
AFE:ON SR:16K
I2C:OK OLED:3C
SD:OK
```

## EV (last-event) codes (standardized)

Boot/diag:
- `boot`, `safe-on`, `safe-off`, `rst-wdt`, `rst-panic`, `rst-pwr`, `rst-sw`, `rst-oth`

Network:
- `eth-up`, `eth-down`, `wifi-up`, `wifi-down`, `ip-got`, `net-off`

HA WebSocket:
- `ws-up`, `ws-down`, `auth-ok`, `auth-bad`, `stt-bin`

MQTT:
- `mqtt-up`, `mqtt-down`

Pipeline:
- `wake`, `vad-start`, `vad-end`, `run-start`, `intent-end`, `tts-start`, `tts-dl`, `tts-play`, `tts-done`

OTA:
- `ota-start`, `ota-ok`, `ota-fail`

Music:
- `mus-play`, `mus-stop`, `mus-pause`, `mus-resume`

Rules:
- Only update EV on meaningful transitions (avoid spamming).
- During `SAFE` or `OTA:RUN`, keep EV as the most relevant cause (`safe-on`, `ota-start`, etc).

## LED readiness alignment (UX decision)

Goal: Do not show “ready/green” when HA is unavailable.

Priority order:
`ERROR(SAFE)` > `OTA` > `SPEAKING` > `PROCESSING` > `LISTENING` > `CONNECTING` > `IDLE`

Idle behavior:
- `IDLE` (green) only when:
  - Wake word mode is running **AND**
  - `HA:OK` (ws connected + authenticated)
- `CONNECTING` (blue breathing) when:
  - Wake word mode is running **AND**
  - `HA:NO` (regardless of whether root cause is `NET:OFF` or HA WS down)
- MQTT does not gate “ready” (OLED shows `MQ:NO` if needed).

Hysteresis (anti-flicker):
- Switch to `CONNECTING` only after ~`3s` continuous `HA:NO`
- Switch back to `IDLE` after ~`1s` continuous `HA:OK`
