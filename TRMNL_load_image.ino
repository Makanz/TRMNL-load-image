#include <Arduino.h>
#include "driver.h"
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <base64.hpp>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <esp_sleep.h>


#include "config.h"
#include "secrets.h"

EPaper epd = EPaper();

const int BUTTON_1 = D1;

String storedChecksum = "";
String previousChecksum = "";
bool hasValidImage = false;
uint32_t wakeCounter = 0;

const uint64_t SLEEP_DURATION_US = (uint64_t)REFRESH_INTERVAL_MINUTES * 60ULL * 1000000ULL;
const uint32_t FULL_FETCH_WAKE_COUNT = FULL_FETCH_INTERVAL_HOURS * 60 / REFRESH_INTERVAL_MINUTES;
#define EEPROM_CHECKSUM_MAX_LEN    71   // "sha256:" (7) + 64 hex digits
#define EEPROM_WAKE_COUNTER_OFFSET 76   // 72 bytes for checksum+null, 4-byte aligned

struct ImageChange {
  int x;
  int y;
  int width;
  int height;
};

struct ImageDiffResult {
  ImageChange changes[10];
  int changeCount;
  String currentChecksum;
  String previousChecksum;
};

// Y-positioner för varje rad (förberäknade)
#define ROW_TITLE   20
#define ROW_SSID    66
#define ROW_STATUS  102
#define ROW_IP      138
#define ROW_SIGNAL  174
#define ROW_BSSID   210
#define ROW_CHANNEL 246
#define ROW_H       36   // radhöjd för partial update

bool isFirstDraw = true;
static int  partialUpdateCount = 0;
static const int FULL_REFRESH_EVERY = 20;

// Föregående värden per rad
static wl_status_t prevWifiStatus = (wl_status_t)-1;
static String prevIP      = "";
static int    prevRSSI    = 0;
static int    prevChannel = 0;
static String prevBSSID   = "";

String rssiQuality(int rssi) {
  if (rssi >= -50) return "Utmarkt";
  if (rssi >= -65) return "Bra";
  if (rssi >= -75) return "Svag";
  return "Dalig";
}

bool wifiStatusChanged(wl_status_t& status, String& ip, int& rssi, int& channel, String& bssid) {
  status  = WiFi.status();
  rssi    = (status == WL_CONNECTED) ? WiFi.RSSI()    : 0;
  ip      = (status == WL_CONNECTED) ? WiFi.localIP().toString() : "";
  channel = (status == WL_CONNECTED) ? WiFi.channel() : 0;
  bssid   = (status == WL_CONNECTED) ? WiFi.BSSIDstr(): "";

  return (status != prevWifiStatus) ||
         (ip != prevIP) ||
         (abs(rssi - prevRSSI) >= 3) ||
         (channel != prevChannel) ||
         (bssid != prevBSSID);
}

// Tvåpassstrategi mot ghosting:
// Pass 1 – rent vitt → talar om för displayen vad som ska raderas
// Pass 2 – ny text → ritar faktiskt innehåll
void updateRow(int y, const String& text, int textSize = 2) {
  // Pass 1: radera
  epd.fillRect(0, y, SCREEN_WIDTH, ROW_H, TFT_WHITE);
  epd.updataPartial(0, y, SCREEN_WIDTH, ROW_H);

  // Pass 2: rita ny text (hoppa över om raden ska vara tom)
  if (text.length() > 0) {
    epd.setTextColor(TFT_BLACK);
    epd.setTextSize(textSize);
    epd.drawString(text, 20, y);
    epd.updataPartial(0, y, SCREEN_WIDTH, ROW_H);
  }

  partialUpdateCount++;
  if (partialUpdateCount >= FULL_REFRESH_EVERY) {
    isFirstDraw = true;     // tvinga full refresh vid nästa anrop
    partialUpdateCount = 0;
  }
}

void drawWiFiStatusScreen() {
  wl_status_t status; String ip, bssid; int rssi, channel;
  if (!wifiStatusChanged(status, ip, rssi, channel, bssid) && !isFirstDraw) return;

  String statusStr;
  if      (status == WL_CONNECTED)      statusStr = "Ansluten";
  else if (status == WL_DISCONNECTED)   statusStr = "Frånkopplad";
  else if (status == WL_CONNECT_FAILED) statusStr = "Anslutning misslyckades";
  else if (status == WL_NO_SSID_AVAIL)  statusStr = "SSID ej hittad";
  else                                   statusStr = "Ansluter...";

  if (isFirstDraw) {
    // Initialt: full clear + full update (en gång)
    epd.fillScreen(TFT_WHITE);
    epd.setTextColor(TFT_BLACK);
    epd.setTextSize(3);
    epd.drawString("WiFi Status", 20, ROW_TITLE);
    epd.setTextSize(2);
    epd.drawString("SSID: " + String(WIFI_SSID), 20, ROW_SSID);
    epd.drawString("Status: " + statusStr, 20, ROW_STATUS);
    if (status == WL_CONNECTED) {
      epd.drawString("IP: " + ip, 20, ROW_IP);
      epd.drawString("Signal: " + String(rssi) + " dBm (" + rssiQuality(rssi) + ")", 20, ROW_SIGNAL);
      epd.drawString("BSSID: " + bssid, 20, ROW_BSSID);
      epd.drawString("Kanal: " + String(channel), 20, ROW_CHANNEL);
    }
    epd.update();
    isFirstDraw = false;
  } else {
    // Partiell uppdatering – endast ändrade rader
    if (status != prevWifiStatus)
      updateRow(ROW_STATUS, "Status: " + statusStr);
    if (ip != prevIP)
      updateRow(ROW_IP, ip.length() > 0 ? "IP: " + ip : "");
    if (abs(rssi - prevRSSI) >= 3)
      updateRow(ROW_SIGNAL, rssi != 0 ? "Signal: " + String(rssi) + " dBm (" + rssiQuality(rssi) + ")" : "");
    if (bssid != prevBSSID)
      updateRow(ROW_BSSID, bssid.length() > 0 ? "BSSID: " + bssid : "");
    if (channel != prevChannel)
      updateRow(ROW_CHANNEL, channel != 0 ? "Kanal: " + String(channel) : "");
  }

  prevWifiStatus = status;
  prevIP         = ip;
  prevRSSI       = rssi;
  prevChannel    = channel;
  prevBSSID      = bssid;
}

void connectWiFi() {
  Serial.println("Connecting to WiFi...");

  const int MAX_RETRIES       = 5;   // antal fullständiga försök
  const int POLL_INTERVAL_MS  = 500; // hur ofta vi kollar status per försök
  const int POLLS_PER_ATTEMPT = 20;  // 20 × 500 ms = 10 s per försök
  const int RETRY_DELAY_MS    = 3000;// paus mellan försök (låter routern "svalna")

  WiFi.mode(WIFI_STA);

  for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    Serial.printf("Attempt %d/%d\n", attempt, MAX_RETRIES);

    WiFi.disconnect(true);
    delay(500);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    for (int poll = 0; poll < POLLS_PER_ATTEMPT; poll++) {
      if (WiFi.status() == WL_CONNECTED) break;
      delay(POLL_INTERVAL_MS);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi connected");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
      digitalWrite(LED_BUILTIN, HIGH);
      return;
    }

    Serial.printf("Attempt %d failed (status %d)", attempt, WiFi.status());
    if (attempt < MAX_RETRIES) {
      Serial.printf(", retrying in %d s...\n", RETRY_DELAY_MS / 1000);
      delay(RETRY_DELAY_MS);
    } else {
      Serial.println();
    }
  }

  Serial.println("WiFi connection failed!");
  digitalWrite(LED_BUILTIN, LOW);
}

void reconnectWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, reconnecting...");
    digitalWrite(LED_BUILTIN, LOW);
    connectWiFi();
  }
}

String getBasicAuthHeader() {
  static String cached;
  if (cached.length() > 0) return cached;

  String auth = String(BASIC_AUTH_USER) + ":" + String(BASIC_AUTH_PASS);
  unsigned int encodedLen = encode_base64_length(auth.length());
  unsigned char encoded[encodedLen + 1];
  encode_base64((unsigned char*)auth.c_str(), auth.length(), encoded);
  encoded[encodedLen] = '\0';
  cached = String("Basic ") + String((char*)encoded);
  return cached;
}

void clearChecksumInEEPROM() {
  EEPROM.write(0, '\0');
  EEPROM.commit();
}

void saveChecksumToEEPROM(const String& checksum) {
  for (int i = 0; i < EEPROM_CHECKSUM_MAX_LEN && i < (int)checksum.length(); i++) EEPROM.write(i, checksum[i]);
  EEPROM.write(EEPROM_CHECKSUM_MAX_LEN, '\0');
  EEPROM.commit();
}

void saveWakeCounterToEEPROM(uint32_t counter) {
  EEPROM.write(EEPROM_WAKE_COUNTER_OFFSET, (counter >> 24) & 0xFF);
  EEPROM.write(EEPROM_WAKE_COUNTER_OFFSET + 1, (counter >> 16) & 0xFF);
  EEPROM.write(EEPROM_WAKE_COUNTER_OFFSET + 2, (counter >> 8) & 0xFF);
  EEPROM.write(EEPROM_WAKE_COUNTER_OFFSET + 3, counter & 0xFF);
  EEPROM.commit();
}

uint32_t loadWakeCounterFromEEPROM() {
  uint32_t c = 0;
  c = (EEPROM.read(EEPROM_WAKE_COUNTER_OFFSET) << 24) |
       (EEPROM.read(EEPROM_WAKE_COUNTER_OFFSET + 1) << 16) |
       (EEPROM.read(EEPROM_WAKE_COUNTER_OFFSET + 2) << 8) |
       EEPROM.read(EEPROM_WAKE_COUNTER_OFFSET + 3);
  return c;
}

String loadChecksumFromEEPROM() {
  String checksum = ""; char c;
  for (int i = 0; i < EEPROM_CHECKSUM_MAX_LEN; i++) { c = EEPROM.read(i); if (c == '\0') break; checksum += c; }
  return checksum;
}

String fetchChecksum() {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, API_URL_META);
  http.setTimeout(10000);
  http.addHeader("Authorization", getBasicAuthHeader());
  http.addHeader("Accept", "application/json");

  Serial.println("Fetching checksum...");
  Serial.printf("[Heap] Fore checksum-fetch: %u bytes fritt\n", ESP.getFreeHeap());

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Checksum HTTP error: %d\n", code);
    http.end();
    return "";
  }

  String payload = http.getString();
  http.end();
  Serial.printf("[Heap] Efter checksum-fetch: %u bytes fritt, payload: %d bytes\n", ESP.getFreeHeap(), payload.length());
  Serial.printf("Checksum raw response: %s\n", payload.c_str());

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("Checksum JSON error: %s\n", err.c_str());
    return "";
  }

  return doc["checksum"] | "";
}

ImageDiffResult fetchImageDiff() {
  ImageDiffResult result = {{}, 0, "", ""};
  
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  
  String url = String(API_URL_DIFF);
  Serial.printf("Fetching image diff from: %s\n", url.c_str());
  http.begin(client, url);
  http.setTimeout(10000);
  http.addHeader("Authorization", getBasicAuthHeader());
  http.addHeader("Accept", "application/json");

  Serial.println("Fetching image diff...");
  Serial.printf("[Heap] Before diff-fetch: %u bytes free\n", ESP.getFreeHeap());

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("ImageDiff HTTP error: %d\n", code);
    http.end();
    return result;
  }

  String payload = http.getString();
  http.end();
  Serial.printf("[Heap] After diff-fetch: %u bytes free, payload: %d bytes\n", ESP.getFreeHeap(), payload.length());
  Serial.printf("Raw response: %s\n", payload.c_str());

  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("ImageDiff JSON error: %s\n", err.c_str());
    return result;
  }

  JsonObject root;
  if (doc.is<JsonArray>()) {
    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() > 0) {
      root = arr[0].as<JsonObject>();
    }
  } else {
    root = doc.as<JsonObject>();
  }

  if (root.isNull()) {
    Serial.println("No valid JSON object in response");
    return result;
  }

  JsonArray changes = root["changes"];
  result.currentChecksum = root["currentChecksum"] | "";
  result.previousChecksum = root["previousChecksum"] | "";
  
  Serial.printf("Current checksum: %s\n", result.currentChecksum.c_str());
  Serial.printf("Previous checksum: %s\n", result.previousChecksum.c_str());

  if (changes.isNull()) {
    Serial.println("No changes array in response");
    Serial.printf("Found 0 changes, current: %s\n", result.currentChecksum.c_str());
    return result;
  }

  result.changeCount = 0;
  for (JsonObject change : changes) {
    if (result.changeCount >= 10) break;
    result.changes[result.changeCount].x = change["x"] | 0;
    result.changes[result.changeCount].y = change["y"] | 0;
    result.changes[result.changeCount].width = change["width"] | 0;
    result.changes[result.changeCount].height = change["height"] | 0;
    result.changeCount++;
  }

  Serial.printf("Found %d changes, current: %s\n", result.changeCount, result.currentChecksum.c_str());
  return result;
}

// Decodes a 1-bit BMP stream on-the-fly as data arrives via http.writeToStream().
// No large heap buffer needed — draws each row directly to the display.
class BMPDecodeStream : public Stream {
  uint8_t  _hdr[62];
  uint8_t  _hdrPos   = 0;
  uint32_t _pixelOffset;
  int32_t  _width, _height;
  int32_t  _absHeight;
  int      _rowBytes;
  uint8_t  _row[100]; // max for 800-pixel 1-bit row (word-aligned: (800+31)/32*4 = 100)
  int      _rowPos   = 0;
  int      _rowsDone = 0;
  uint32_t _bytesSeen = 0; // bytes seen after the 62-byte header
  bool     _valid    = false;
  bool     _done     = false;

public:
  bool valid()    const { return _valid; }
  int  rowsDone() const { return _rowsDone; }

  size_t write(const uint8_t* data, size_t sz) override {
    for (size_t i = 0; i < sz; i++) {
      uint8_t b = data[i];

      // Phase 1: accumulate 62-byte header (14 file header + 40 DIB header + 8 color table)
      if (_hdrPos < 62) {
        _hdr[_hdrPos++] = b;
        if (_hdrPos == 62) {
          if (_hdr[0] != 0x42 || _hdr[1] != 0x4D) {
            Serial.println("BMP magic mismatch");
            return sz;
          }
          _pixelOffset = _hdr[10]|(_hdr[11]<<8)|(_hdr[12]<<16)|(_hdr[13]<<24);
          _width  = (int32_t)(_hdr[18]|(_hdr[19]<<8)|(_hdr[20]<<16)|(_hdr[21]<<24));
          _height = (int32_t)(_hdr[22]|(_hdr[23]<<8)|(_hdr[24]<<16)|(_hdr[25]<<24));
          uint16_t bpp  = _hdr[28] | (_hdr[29]<<8);
          uint32_t comp = _hdr[30]|(_hdr[31]<<8)|(_hdr[32]<<16)|(_hdr[33]<<24);
          _absHeight = abs(_height);
          if (bpp != 1 || comp != 0 ||
              _absHeight == 0 || _absHeight > SCREEN_HEIGHT ||
              _width  <= 0   || _width  > SCREEN_WIDTH) {
            Serial.printf("Unsupported BMP: %ldx%ld %dbpp comp=%lu\n", _width, _absHeight, bpp, comp);
            return sz;
          }
          _rowBytes = ((_width + 31) / 32) * 4;
          _valid = true;
          Serial.printf("BMP: %ldx%ld, rowSize=%d, pixelOffset=%lu\n", _width, _absHeight, _rowBytes, _pixelOffset);
          epd.fillScreen(TFT_WHITE);
        }
        continue;
      }

      if (!_valid || _done) continue;

      // Phase 2: skip bytes between end of 62-byte read and actual pixel data offset
      _bytesSeen++;
      if (_bytesSeen <= _pixelOffset - 62) continue;

      // Phase 3: accumulate row bytes, draw when a full row is ready
      _row[_rowPos++] = b;
      if (_rowPos == _rowBytes) {
        int displayY = (_height < 0) ? _rowsDone : (_absHeight - 1 - _rowsDone);
        for (int x = 0; x < _width; x++) {
          bool isWhite = (_row[x >> 3] >> (7 - (x & 7))) & 0x01;
          epd.drawPixel(x, displayY, isWhite ? TFT_WHITE : TFT_BLACK);
        }
        _rowsDone++;
        _rowPos = 0;
        if (_rowsDone >= _absHeight) _done = true;
      }
    }
    return sz;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }
  int available() override { return 0; }
  int read()      override { return -1; }
  int peek()      override { return -1; }
};

bool fetchRawImageAndDisplay() {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, API_URL_IMAGE);
  http.setTimeout(15000);
  http.addHeader("Authorization", getBasicAuthHeader());

  Serial.println("Fetching raw image...");
  Serial.printf("[Heap] Fore bild-fetch: %u bytes fritt\n", ESP.getFreeHeap());


  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Image HTTP error: %d\n", code);
    http.end();
    return false;
  }

  BMPDecodeStream decoder;
  http.writeToStream(&decoder);
  http.end();

  if (!decoder.valid()) {
    Serial.println("BMP decode failed");
    return false;
  }

  Serial.printf("Drew %d rows\n", decoder.rowsDone());
  Serial.println("Calling epd.update()...");
  epd.update();
  Serial.println("BMP rendered successfully");
  return true;
}

void fetchAndDisplayImage(bool isColdBoot) {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi not connected, skipping fetch"); return; }

  bool shouldFullFetch = false;

  if (isColdBoot) {
    Serial.println("Cold boot - fetching full image");
    shouldFullFetch = true;
    wakeCounter = 0;
  } else {
    wakeCounter++;
    saveWakeCounterToEEPROM(wakeCounter);
    Serial.printf("Wake #%u, full fetch every %u\n", wakeCounter, FULL_FETCH_WAKE_COUNT);

    if (wakeCounter >= FULL_FETCH_WAKE_COUNT) {
      Serial.println("Full fetch interval reached");
      shouldFullFetch = true;
      wakeCounter = 0;
    } else {
      Serial.println("Checking for changes...");
      ImageDiffResult diff = fetchImageDiff();
      if (!diff.currentChecksum.isEmpty() && diff.currentChecksum == storedChecksum) {
        Serial.println("No changes detected");
        return;
      }
      shouldFullFetch = true;
    }
  }

  if (shouldFullFetch) {
    Serial.println("Fetching full image...");
    bool success = fetchRawImageAndDisplay();
    if (success) {
      ImageDiffResult diff = fetchImageDiff();
      storedChecksum = diff.currentChecksum;
      previousChecksum = diff.previousChecksum;
      saveChecksumToEEPROM(storedChecksum);
      saveWakeCounterToEEPROM(wakeCounter);
      hasValidImage = true;
    }
  }
}

void drawInitialScreen() {
  epd.fillScreen(TFT_WHITE);
  epd.setTextColor(TFT_BLACK);
  epd.setTextSize(2);
  epd.drawString("Connecting to WiFi...", 20, 20);
  epd.update();
}

void drawErrorScreen(const String& errorMsg) {
  epd.fillScreen(TFT_WHITE);
  epd.setTextColor(TFT_BLACK);
  epd.setTextSize(2);
  epd.drawString("Error:", 20, 20);
  epd.drawString(errorMsg, 20, 50);
  epd.update();
}

void goToSleep() {
  Serial.printf("Går in i deep sleep i %d minuter...\n", REFRESH_INTERVAL_MINUTES);
  Serial.flush();
  // esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
  // esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  // Vänta max 3 s på Serial – blockerar inte fristående enhet
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}

  bool isTimerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;

  Serial.println(isTimerWake ? "Vaknat från deep sleep" : "TRMNL Uppstart");
  Serial.println("=====================");

  if (!EEPROM.begin(128)) {
    Serial.println("EEPROM.begin failed!");
    epd.fillScreen(TFT_WHITE);
    epd.setTextColor(TFT_BLACK);
    epd.setTextSize(2);
    epd.drawString("EEPROM error!", 20, 20);
    epd.update();
    delay(5000);
  }
  storedChecksum = loadChecksumFromEEPROM(); // Ladda sparad checksum
  wakeCounter = loadWakeCounterFromEEPROM();

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  pinMode(BUTTON_1, INPUT_PULLUP);
  if (digitalRead(BUTTON_1) == LOW) {
    Serial.println("Button 1 pressed – clearing stored checksum");
    storedChecksum = "";
    previousChecksum = "";
    wakeCounter = 0;
    clearChecksumInEEPROM();
    saveWakeCounterToEEPROM(0);
  }

  epd.init();
  epd.setRotation(0);
  epd.setTextColor(TFT_BLACK);

  if (!isTimerWake) {
    // Endast vid cold boot: visa "Connecting..."-skärm och WiFi-status
    drawInitialScreen();
  }

  connectWiFi();

  if (!isTimerWake) {
    drawWiFiStatusScreen();
    delay(3000);
  }

  Serial.printf("[Heap] Efter WiFi: %u bytes fritt\n", ESP.getFreeHeap());

  if (WiFi.status() == WL_CONNECTED) {
    fetchAndDisplayImage(!isTimerWake);
  } else {
    Serial.println("WiFi misslyckades, sover ändå.");
  }

  goToSleep();
}

void loop() {
  // Når aldrig hit – deep sleep i setup() startar om enheten vid uppvakning
}
