#ifndef FIRMWARE_STATE_H
#define FIRMWARE_STATE_H

#include <Arduino.h>
#include "config.h"

constexpr size_t EEPROM_CHECKSUM_MAX_LEN = 71;
constexpr int EEPROM_WAKE_COUNTER_OFFSET = 76;
constexpr int EEPROM_REFRESH_INTERVAL_OFFSET = 80;
constexpr int EEPROM_FULL_FETCH_ELAPSED_OFFSET = 84;
constexpr int EEPROM_LAST_ERROR_CODE_OFFSET = 88;
constexpr int EEPROM_LAST_ERROR_TIMESTAMP_OFFSET = 90;
constexpr int EEPROM_ERROR_COUNT_OFFSET = 94;
constexpr int MAX_IMAGE_CHANGES = 10;
constexpr size_t EEPROM_SIZE = 128;

enum class ErrorCode : uint16_t {
  NONE = 0,
  WIFI_CONNECT_FAILED = 1,
  WIFI_TIMEOUT = 2,
  HTTP_TIMEOUT = 3,
  HTTP_ERROR = 4,
  INVALID_CHECKSUM = 5,
  INVALID_BMP = 6,
  BMP_RENDER_FAILED = 7,
  EEPROM_CORRUPT = 8,
  API_JSON_ERROR = 9,
  API_UNEXPECTED_RESPONSE = 10,
  UNKNOWN = 255
};

struct ImageChange {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct ImageDiffResult {
  ImageChange changes[MAX_IMAGE_CHANGES];
  int changeCount = 0;
  String currentChecksum = "";
  String previousChecksum = "";
  uint32_t refreshIntervalSeconds = 0;
};

struct WiFiStatusSnapshot {
  int8_t status = -1;
  String ip = "";
  int rssi = 0;
  int channel = 0;
  String bssid = "";
};

struct WiFiDisplayState {
  bool isFirstDraw = true;
  int partialUpdateCount = 0;
  int8_t prevWifiStatus = -1;
  String prevIP = "";
  int prevRSSI = 0;
  int prevChannel = 0;
  String prevBSSID = "";
};

struct FirmwareState {
  String storedChecksum = "";
  String previousChecksum = "";
  bool hasValidImage = false;
  uint32_t wakeCounter = 0;
  uint32_t refreshIntervalSeconds = 0;
  uint32_t elapsedFullFetchSeconds = 0;
  ErrorCode lastErrorCode = ErrorCode::NONE;
  uint32_t lastErrorTimestamp = 0;
  uint16_t errorOccurrenceCount = 0;
  WiFiDisplayState wifiDisplay;
};

uint32_t getDefaultRefreshInterval();
uint32_t getFullFetchIntervalSeconds();
bool isRefreshIntervalValid(uint32_t intervalSeconds);

inline uint32_t getDefaultRefreshInterval() {
  return (uint32_t)REFRESH_INTERVAL_MINUTES * 60ULL;
}

inline uint32_t getFullFetchIntervalSeconds() {
  return (uint32_t)FULL_FETCH_INTERVAL_HOURS * 3600ULL;
}

inline bool isRefreshIntervalValid(uint32_t intervalSeconds) {
  return intervalSeconds >= REFRESH_INTERVAL_MIN_SECONDS && intervalSeconds <= REFRESH_INTERVAL_MAX_SECONDS;
}

#endif
