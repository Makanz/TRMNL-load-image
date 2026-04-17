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

`setup()` contains all runtime logic; `loop()` is never reached. The device either cold-boots or wakes via `ESP_SLEEP_WAKEUP_TIMER`. At the end of `setup()`, `goToSleep()` puts the ESP32 back into deep sleep for the configured interval.

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
| 88 | 2 | Last error code (big-endian `uint16_t`) |
| 90 | 4 | Last error timestamp (big-endian `uint32_t`) |
| 94 | 2 | Error occurrence count (big-endian `uint16_t`) |

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

## Error Handling & Recovery

The firmware implements comprehensive error handling with automatic retries and graceful degradation:

### Image Fetch Retries

When a full image fetch fails, the device automatically retries up to 3 times with exponential backoff:
- **Attempt 1**: Immediate
- **Attempt 2**: After 1 second delay
- **Attempt 3**: After 2 second delay

If all retries fail, the error is logged to EEPROM and displayed on the ePaper screen as a 4-character error code.

### Error Codes

Error codes are displayed in the top-right corner of the ePaper screen during failures:

| Code | Error | Recovery |
|------|-------|----------|
| E001 | WiFi connection failed | Check SSID/password in `secrets.h`; retries on next wake |
| E002 | WiFi timeout | WiFi issue; device sleeps and retries |
| E003 | HTTP request timeout | Network connectivity issue; retries automatically |
| E004 | HTTP error (non-200) | API server issue or authentication failure |
| E005 | Invalid checksum format | Checksum corruption detected; cleared and fetches fresh |
| E006 | Invalid BMP header | Corrupted image data; retries automatically |
| E007 | BMP rendering failed | Display issue; retries on next wake |
| E008 | EEPROM corruption | EEPROM values out of range; cleared automatically |
| E009 | JSON parsing error | API response format issue; retries automatically |
| ERRA | Unexpected API response | API response missing required fields; retries |
| ERRX | Unknown error | Unclassified failure; retries on next wake |

### Sleep Interval Behavior

- **Success**: Device sleeps for configured `REFRESH_INTERVAL_MINUTES` (default 1 minute)
- **Error**: Device sleeps but error is logged to EEPROM for diagnostics
- **Extended failure**: API-provided `refreshInterval` is respected when present

### State Validation

On boot, the firmware validates all persisted state:
- **Checksums**: Invalid characters detected → cleared automatically
- **Refresh intervals**: Out of bounds → reset to default
- **Elapsed timers**: Exceeds maximum → reset to 0

### Serial Diagnostics

The firmware prints comprehensive diagnostics to the serial port (115200 baud):
- Boot information: Wake count, stored checksum, last error
- WiFi connection progress and attempt details
- Image fetch attempts and retry counts
- Error codes with timestamps
- Heap usage before/after WiFi and image fetch
- Final shutdown status

Enable serial monitoring in the Arduino IDE to debug issues.

## Troubleshooting

- **WiFi fails (E001)**: Check `secrets.h` SSID/password; ensure AP is in range; check serial output for WiFi status details
- **Display shows "EEPROM error!"**: EEPROM initialization failed — board may need a power cycle
- **Error code on screen (E002-ERRX)**: Check serial monitor at 115200 baud for detailed error logs; errors are retried automatically on next wake
- **No image on screen after WiFi connects**: Check webhook URL in `config.h`; verify API server is reachable; check serial output for HTTP errors
- **Button 1 during boot**: Press to clear EEPROM state (wake counter, checksum, errors) and force a full refresh on next cycle
- **Frequent E006 errors**: BMP image format issue; verify API server returns valid 8-bit monochrome BMP
- **Frequent E009 errors**: JSON parsing issue; verify API response structure matches expected format
- **Serial output shows errors but no display message**: Device is retrying; give it time to complete retries before next wake
