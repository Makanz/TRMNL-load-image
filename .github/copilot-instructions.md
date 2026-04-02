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
| `TRMNL_load_image.ino` | All firmware logic |
| `config.h` | API endpoints, screen dimensions, refresh intervals |
| `driver.h` | Board/screen selection defines (included by TFT_eSPI) |
| `secrets.h` | WiFi credentials & API Basic Auth (gitignored) |
| `secrets.h.example` | Template — copy to `secrets.h` and fill in values |

## Architecture

### Wake/sleep cycle
`setup()` contains all runtime logic; `loop()` is never reached. The device either cold-boots or wakes via `ESP_SLEEP_WAKEUP_TIMER`. At the end of `setup()`, `goToSleep()` puts the ESP32 back into deep sleep for `REFRESH_INTERVAL_MINUTES`.

### Image update strategy
Two modes are used to minimize ePaper wear and data transfer:

1. **Partial update** (most wakes): Calls `?type=imageDiff` with the stored checksum, receives changed regions (x/y/width/height), then fetches each region via `?type=imageRegion` as base64-encoded PNG.
2. **Full update** (cold boot, or every `FULL_FETCH_INTERVAL_HOURS`): Downloads the full image via `?type=image` (raw PNG or base64 PNG), decodes and renders it, then calls `?type=imageDiff` to capture the current checksum.

### EEPROM layout
- Bytes 0–62: null-terminated checksum string (tracks last known image state)
- Bytes 64–67: `wakeCounter` as big-endian `uint32_t` (`EEPROM_WAKE_COUNTER_OFFSET = 64`)

### PNG rendering
`PNGDraw()` is the per-line callback for `PNGdec`. It converts RGB565 pixels to monochrome using luminance weighting (`lum = r*77 + g*150 + b*29 >> 8`) with threshold 128. `regionOffsetX`/`regionOffsetY` are globals set before each decode so partial regions land at the correct screen position.

### Anti-ghosting for ePaper
Partial updates use a two-pass strategy: first erase the row to white (`fillRect` + `updataPartial`), then draw new content (`drawString` + `updataPartial`). After `FULL_REFRESH_EVERY` (20) partial updates, a full `epd.update()` is triggered to prevent ghosting.

### HTTP / chunked encoding
`BufStream` (a `Stream` subclass with dynamic buffer) is used with `http.writeToStream()` for the full image fetch. This correctly handles `Transfer-Encoding: chunked` — do **not** use `http.getStreamPtr()` for this, as it returns raw TCP data including chunk-size headers.

## Key Conventions

- All HTTP requests use `WiFiClientSecure` with `client.setInsecure()` (no cert verification).
- Basic Auth header is base64-encoded once and cached statically in `getBasicAuthHeader()`.
- API responses can be either a JSON object or a single-element JSON array — `fetchImageDiff()` handles both.
- Button 1 (`D1`, active-low with `INPUT_PULLUP`) clears EEPROM state on boot for a forced full refresh.
- UI-facing strings are written in **Swedish** (e.g., status labels, Serial debug messages during WiFi connection).
- `malloc`/`free` is used directly for image buffers; always `free()` and null the pointer to avoid double-free in `BufStream`'s destructor.

## Configuration

Edit `config.h` to change:
- `API_URL` — base webhook URL
- `REFRESH_INTERVAL_MINUTES` — deep sleep duration between wakes
- `FULL_FETCH_INTERVAL_HOURS` — how often to force a full image download

Copy `secrets.h.example` → `secrets.h` and set `WIFI_SSID`, `WIFI_PASSWORD`, `BASIC_AUTH_USER`, `BASIC_AUTH_PASS`.
