# TRMNL_load_image

ESP32 firmware for TRMNL 7.5" monochrome ePaper display (UC8179, 800×480). The device periodically wakes from deep sleep, fetches image content from a webhook API, and renders it on screen.

## Hardware

- **Board**: Seeed XIAO ESP32 with ePaper display (`USE_XIAO_EPAPER_DISPLAY_BOARD_EE04` in `driver.h`)
- **Display**: 7.5-inch monochrome ePaper, `BOARD_SCREEN_COMBO 502`
- **Button 1** (`D1`, active-low): clears EEPROM on boot for forced full refresh

## Setup

1. Copy `secrets.h.example` → `secrets.h` and fill in your WiFi SSID, WiFi password, and API Basic Auth credentials
2. Open `TRMNL_load_image.ino` in the Arduino IDE
3. Install required libraries:
   - TFT_eSPI
   - EEPROM (built-in)
   - WiFi (built-in)
   - HTTPClient (built-in)
   - WiFiClientSecure (built-in)
   - ArduinoJson
   - base64
4. Upload at **115200 baud**

## Architecture

### Wake/sleep cycle

`setup()` contains all runtime logic; `loop()` is never reached. The device either cold-boots or wakes via `ESP_SLEEP_WAKEUP_TIMER`. The display is initialized lazily only when the firmware actually needs to draw something, so timer wakes that detect no content change can skip ePaper startup. At the end of `setup()`, `goToSleep()` puts the ESP32 back into deep sleep for the configured interval.

### Image update

- **Cold boot**: full image download via `?type=image` (BMP format), decoded and rendered via `BMPDecodeStream`. After rendering, `?type=imageDiff` is called to capture the current checksum.
- **Timer wakes**: the firmware accumulates slept seconds and forces a full image fetch once `FULL_FETCH_INTERVAL_HOURS` has elapsed since the last successful full refresh.
- **Between forced full refreshes**: timer wakes call `?type=imageDiff`; if the checksum changed, the device performs a full BMP fetch immediately and resets the 4-hour full-refresh timer.

### EEPROM layout (128 bytes total)

| Offset | Size | Content |
|--------|------|---------|
| 0 | 71 | Null-terminated checksum string |
| 76 | 4 | `wakeCounter` (big-endian `uint32_t`) |
| 80 | 4 | `refreshIntervalSeconds` (big-endian `uint32_t`) |
| 84 | 4 | Elapsed seconds since last successful full refresh |

EEPROM writes are batched so related state changes are committed together instead of issuing separate commits per field.

### Key files

| File | Purpose |
|---|---|
| `TRMNL_load_image.ino` | Entry point, sleep orchestration, button handling |
| `image_api.cpp/.h` | HTTP requests for checksum, diff, raw BMP; `BMPDecodeStream` integration |
| `display_ui.cpp/.h` | Splash screen, WiFi status with partial updates, error screen |
| `storage.cpp/.h` | EEPROM read/write for checksum, wake counter, refresh interval |
| `wifi_manager.cpp/.h` | WiFi connection with retry logic, RSSI quality, status snapshot |
| `bmp_decode.cpp/.h` | Stream-based BMP decoder for direct image rendering |
| `firmware_state.h` | Core structs and EEPROM layout constants |

## Configuration

Edit `config.h`:

| Parameter | Default | Description |
|---|---|---|
| `API_URL` | (webhook URL) | Base webhook URL |
| `REFRESH_INTERVAL_MINUTES` | 1 | Deep sleep duration between wakes |
| `FULL_FETCH_INTERVAL_HOURS` | 4 | Exact max time between successful full image downloads |
| `REFRESH_INTERVAL_MIN_SECONDS` | 60 | Minimum API-provided refresh override |
| `REFRESH_INTERVAL_MAX_SECONDS` | 14400 | Maximum API-provided refresh override (4 hours) |

## API Endpoints

The firmware calls four webhook endpoints (all require Basic Auth):

| Endpoint | Purpose |
|---|---|
| `?type=meta` | Returns current image checksum |
| `?type=image` | Returns full image as BMP |
| `?type=imageDiff` | Returns changed regions and current/previous checksums |
| `?type=imageRegion` | Reserved for partial region fetching (not currently used) |

## Troubleshooting

- **WiFi fails**: Check `secrets.h` credentials; ensure AP is in range
- **Display shows "EEPROM error!"**: EEPROM initialization failed — board may need a power cycle
- **No image on screen after WiFi connects**: Check webhook URL in `config.h`; verify API server is reachable
- **Button 1**: Press during boot to clear EEPROM state and force a full refresh on next wake
