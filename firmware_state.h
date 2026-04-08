#ifndef FIRMWARE_STATE_H
#define FIRMWARE_STATE_H

#include <Arduino.h>
#include <WiFi.h>

constexpr size_t EEPROM_CHECKSUM_MAX_LEN = 71;
constexpr int EEPROM_WAKE_COUNTER_OFFSET = 76;
constexpr int MAX_IMAGE_CHANGES = 10;

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
};

struct WiFiStatusSnapshot {
  wl_status_t status = WL_DISCONNECTED;
  String ip = "";
  int rssi = 0;
  int channel = 0;
  String bssid = "";
};

struct WiFiDisplayState {
  bool isFirstDraw = true;
  int partialUpdateCount = 0;
  wl_status_t prevWifiStatus = (wl_status_t)-1;
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
  WiFiDisplayState wifiDisplay;
};

#endif
