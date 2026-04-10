# Copilot Instructions

## Project Overview

Arduino/ESP32 firmware for a TRMNL ePaper display device. The device periodically wakes from deep sleep, fetches image content from a webhook API, and renders it on a 7.5-inch monochrome ePaper screen (UC8179, 800×480).

## Hardware & Platform

- **Board**: Seeed XIAO ESP32 with ePaper display (`USE_XIAO_EPAPER_DISPLAY_BOARD_EE04` in `driver.h`)
- **Display**: 7.5-inch monochrome ePaper, `BOARD_SCREEN_COMBO 502`
- **IDE**: Arduino IDE (no Makefile/CMake; open `.ino` in Arduino IDE to build/upload)
- **Serial monitor**: 115200 baud

## File Structure

| File | Purpose |
|---|---|
| `TRMNL_load_image.ino` | Entry point — `setup()` contains all runtime logic, `loop()` is never reached |
| `config.h` | API endpoint URLs, screen dimensions, refresh intervals |
| `driver.h` | Board/screen selection defines (included by TFT_eSPI) |
| `secrets.h` | WiFi credentials & API Basic Auth (gitignored; copy from `secrets.h.example`) |
| `secrets.h.example` | Template — copy to `secrets.h` and fill in values |
| `firmware_state.h` | Core structs (`FirmwareState`, `ImageDiffResult`, `WiFiStatusSnapshot`, etc.) and EEPROM layout constants |
| `storage.cpp/.h` | EEPROM read/write for checksum, wake counter, refresh interval |
| `wifi_manager.cpp/.h` | WiFi connection with retry logic, RSSI quality lookup, status snapshot |
| `image_api.cpp/.h` | HTTP requests for checksum, diff, and raw BMP image; `BMPDecodeStream` integration |
| `display_ui.cpp/.h` | Screen rendering: initial splash, WiFi status with partial updates, error screen |
| `bmp_decode.cpp/.h` | Stream-based BMP decoder (`BMPDecodeStream`) for direct image rendering |

## Architecture

### Wake/sleep cycle
`setup()` contains all runtime logic; `loop()` is never reached. The device either cold-boots or wakes via `ESP_SLEEP_WAKEUP_TIMER`. At the end of `setup()`, `goToSleep()` puts the ESP32 back into deep sleep for `REFRESH_INTERVAL_MINUTES` (or the API-provided `refreshIntervalSeconds`).

### Image update strategy
Two modes are used to minimize ePaper wear and data transfer:

1. **Full update** (cold boot, or every `FULL_FETCH_INTERVAL_HOURS`): Downloads the full image via `?type=image` (BMP format), decoded and rendered via `BMPDecodeStream`. After rendering, calls `?type=imageDiff` to capture the current checksum.
2. **Full update** (other timer wakes): Currently all timer wakes trigger a full image fetch. The `?type=imageRegion` endpoint and partial region logic was explored but is not active in the current codebase.

### EEPROM layout (128 bytes total)
- Bytes 0–70: null-terminated checksum string (`EEPROM_CHECKSUM_MAX_LEN = 71`)
- Bytes 76–79: `wakeCounter` as big-endian `uint32_t` (`EEPROM_WAKE_COUNTER_OFFSET = 76`)
- Bytes 80–83: `refreshIntervalSeconds` as big-endian `uint32_t` (`EEPROM_REFRESH_INTERVAL_OFFSET = 80`)

### BMP rendering
`BMPDecodeStream` is a `Stream` subclass used with `http.writeToStream()` for the full image fetch. It parses BMP headers from the stream, converts RGB pixels to monochrome using luminance weighting (`lum = r*77 + g*150 + b*29 >> 8`) with threshold 128, and renders directly to the ePaper display. This correctly handles `Transfer-Encoding: chunked` — do **not** use `http.getStreamPtr()` for this, as it returns raw TCP data including chunk-size headers.

### Anti-ghosting for ePaper
`display_ui.cpp` tracks `wifiDisplay.partialUpdateCount` in `FirmwareState`. After `FULL_REFRESH_EVERY` (20) partial WiFi-status updates, `state.wifiDisplay.isFirstDraw` is set to `true` to trigger a full `epd.update()` on next WiFi status change, preventing ghosting on the ePaper.

### HTTP / chunked encoding
`BMPDecodeStream` (a `Stream` subclass) is used with `http.writeToStream()` for the full image fetch. This correctly handles `Transfer-Encoding: chunked` — do **not** use `http.getStreamPtr()` for this, as it returns raw TCP data including chunk-size headers.

## Key Conventions

- All HTTP requests use `WiFiClientSecure` with `client.setInsecure()` (no cert verification).
- Basic Auth header is base64-encoded once and cached statically in `getBasicAuthHeader()`.
- API responses for `imageDiff` can be either a JSON object or a single-element JSON array — `fetchImageDiff()` handles both.
- Button 1 (`D1`, active-low with `INPUT_PULLUP`) clears EEPROM state on boot for a forced full refresh.
- UI-facing strings are written in **Swedish** (e.g., status labels, Serial debug messages during WiFi connection).
- `malloc`/`free` is used directly for image buffers in `BMPDecodeStream`; the stream destructor and `fetchRawImageAndDisplay()` ensure pointers are nulled after `free()` to avoid double-free.
- `wifiManager` LED (`LED_BUILTIN`) indicates WiFi status: LOW on failure, HIGH on success.

## Configuration

Edit `config.h` to change:
- `API_URL` — base webhook URL (and derived `API_URL_META`, `API_URL_IMAGE`, `API_URL_DIFF`, `API_URL_REGION`)
- `REFRESH_INTERVAL_MINUTES` — deep sleep duration between wakes
- `FULL_FETCH_INTERVAL_HOURS` — how often to force a full image download
- `REFRESH_INTERVAL_MIN_SECONDS` / `REFRESH_INTERVAL_MAX_SECONDS` — bounds for API-provided `refreshInterval` override

Copy `secrets.h.example` → `secrets.h` and set `WIFI_SSID`, `WIFI_PASSWORD`, `BASIC_AUTH_USER`, `BASIC_AUTH_PASS`.