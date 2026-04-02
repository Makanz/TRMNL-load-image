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
#define MAX_IMAGE_WIDTH 800
#include <PNGdec.h>

#include "config.h"
#include "secrets.h"

EPaper epd = EPaper();
PNG png;

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
static int partialUpdateCount = 0;
const int FULL_REFRESH_EVERY = 20;

// Föregående värden per rad
static wl_status_t prevWifiStatus = (wl_status_t)-1;
static String prevIP      = "";
static int    prevRSSI    = 0;
static int    prevChannel = 0;
static String prevBSSID   = "";

static int regionOffsetX = 0;
static int regionOffsetY = 0;

int PNGDraw(PNGDRAW* pDraw) {
  uint16_t lineBuffer[MAX_IMAGE_WIDTH];
  png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_BIG_ENDIAN, 0xFFFFFFFF);
  
  for (int x = 0; x < pDraw->iWidth; x++) {
    uint16_t p = lineBuffer[x];
    uint8_t r = (p >> 8) & 0xF8;
    uint8_t g = (p >> 3) & 0xFC;
    uint8_t b = (p << 3) & 0xF8;
    uint8_t lum = (r * 77 + g * 150 + b * 29) >> 8;
    epd.drawPixel(x + regionOffsetX, pDraw->y + regionOffsetY, lum >= 128 ? TFT_WHITE : TFT_BLACK);
  }
  return 1;
}

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

ImageDiffResult fetchImageDiff(const String& previousChecksumParam) {
  ImageDiffResult result = {{}, 0, "", ""};
  
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  
  String url = String(API_URL_DIFF);
  if (previousChecksumParam.length() > 0) {
    url += "&previous=" + previousChecksumParam;
  }
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

String fetchRegionImage(int x, int y, int width, int height) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  
  String url = String(API_URL_REGION) + "&x=" + String(x) + "&y=" + String(y) + 
              "&w=" + String(width) + "&h=" + String(height);
  http.begin(client, url);
  http.setTimeout(15000);
  http.addHeader("Authorization", getBasicAuthHeader());
  http.addHeader("Accept", "application/json");

  Serial.printf("Fetching region (%d,%d,%d,%d)...\n", x, y, width, height);
  Serial.printf("[Heap] Before region-fetch: %u bytes free\n", ESP.getFreeHeap());

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Region HTTP error: %d\n", code);
    http.end();
    return "";
  }

  String payload = http.getString();
  http.end();
  Serial.printf("[Heap] After region-fetch: %u bytes free, payload: %d bytes\n", ESP.getFreeHeap(), payload.length());

  DynamicJsonDocument doc(payload.length() + 512);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("Region JSON error: %s\n", err.c_str());
    return "";
  }

  JsonObject root;
  if (doc.is<JsonArray>()) {
    JsonArray arr = doc.as<JsonArray>();
    if (arr.size() > 0) root = arr[0].as<JsonObject>();
  } else {
    root = doc.as<JsonObject>();
  }
  if (root.isNull()) return "";

  String imageData = root["image"] | "";
  return imageData;
}

bool fetchAndRenderRegion(int x, int y, int width, int height) {
  // UC8179 operates on 8-pixel (1-byte) horizontal boundaries. Align the
  // clear and refresh areas to avoid boundary artifacts. The PNG is still
  // decoded at the original position — only the erase and update window expand.
  int adjX = (x / 8) * 8;
  int adjW = (((x + width  + 7) / 8) * 8) - adjX;
  int adjY = (y / 8) * 8;
  int adjH = (((y + height + 7) / 8) * 8) - adjY;

  String imageData = fetchRegionImage(x, y, width, height);
  if (imageData.isEmpty()) {
    Serial.println("Failed to fetch region image");
    return false;
  }
  
  Serial.printf("[Heap] After region fetch: %u bytes free\n", ESP.getFreeHeap());
  
  if (!imageData.startsWith("data:image/png;base64,")) {
    Serial.println("Invalid image data format");
    return false;
  }
  
  String base64Data = imageData.substring(22);
  
  unsigned int decodedLen = decode_base64_length((unsigned char*)base64Data.c_str(), base64Data.length());
  uint8_t* decodedBuffer = (uint8_t*)malloc(decodedLen);
  if (!decodedBuffer) {
    Serial.println("malloc failed for region decode");
    return false;
  }
  
  int actualLen = decode_base64((unsigned char*)base64Data.c_str(), base64Data.length(), decodedBuffer);
  
  Serial.printf("Region decoded: %d bytes\n", actualLen);
  Serial.printf("[Heap] After region decode: %u bytes free\n", ESP.getFreeHeap());
  
  epd.fillRect(adjX, adjY, adjW, adjH, TFT_WHITE);
  
  regionOffsetX = x;
  regionOffsetY = y;
  
  int16_t rc = png.openRAM(decodedBuffer, actualLen, PNGDraw);
  if (rc != PNG_SUCCESS) {
    Serial.printf("Region PNG open error: %d\n", rc);
    free(decodedBuffer);
    return false;
  }
  
  rc = png.decode(NULL, 0);
  png.close();
  free(decodedBuffer);
  
  if (rc != PNG_SUCCESS) {
    Serial.printf("Region PNG decode error: %d\n", rc);
    return false;
  }
  
  epd.updataPartial(adjX, adjY, adjW, adjH);
  Serial.printf("Rendered region (%d,%d %dx%d) aligned to (%d,%d %dx%d)\n", x, y, width, height, adjX, adjY, adjW, adjH);
  
  return true;
}

bool fetchPartialImage(const ImageDiffResult& diffResult) {
  Serial.printf("Fetching %d changed regions...\n", diffResult.changeCount);
  
  for (int i = 0; i < diffResult.changeCount; i++) {
    const ImageChange& change = diffResult.changes[i];
    Serial.printf("Region %d: (%d,%d) %dx%d\n", i, change.x, change.y, change.width, change.height);
    
    if (!fetchAndRenderRegion(change.x, change.y, change.width, change.height)) {
      Serial.printf("Failed to render region %d\n", i);
      return false;
    }
    
    partialUpdateCount++;
  }
  
  if (partialUpdateCount >= FULL_REFRESH_EVERY) {
    Serial.println("Performing full refresh to prevent ghosting");
    epd.update();
    partialUpdateCount = 0;
  }
  
  return true;
}

// Stream sink that accumulates binary data into a heap buffer.
// Used with http.writeToStream() which correctly decodes chunked encoding,
// unlike getStreamPtr() which returns the raw TCP stream.
class BufStream : public Stream {
public:
  uint8_t* buf = nullptr;
  int      len = 0;
  int      capacity = 0;

  ~BufStream() { free(buf); }

  void clear() {
    free(buf);
    buf = nullptr;
    len = 0;
    capacity = 0;
  }

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* data, size_t sz) override {
    if (len + sz > capacity) {
      int newCap = capacity == 0 ? 256 : capacity * 2;
      while (newCap < len + sz) newCap *= 2;
      uint8_t* nb = (uint8_t*)realloc(buf, newCap);
      if (!nb) return 0;
      buf = nb;
      capacity = newCap;
    }
    memcpy(buf + len, data, sz);
    len += sz;
    return sz;
  }

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

  // writeToStream handles Transfer-Encoding: chunked correctly.
  // getStreamPtr() would return the raw TCP stream including chunk-size headers,
  // causing the PNG magic check to fail.
  BufStream sink;
  http.writeToStream(&sink);
  http.end();

  uint8_t* imgBuffer = sink.buf;
  int      bytesRead = sink.len;

  Serial.printf("[Heap] PNG laddad: %d bytes, fritt: %u bytes\n", bytesRead, ESP.getFreeHeap());

  if (bytesRead == 0) {
    Serial.println("No image data received");
    sink.clear();
    return false;
  }

  // Detect format: PNG magic = 89 50 4E 47
  bool isRawPNG = (bytesRead >= 4 &&
                   imgBuffer[0] == 0x89 && imgBuffer[1] == 0x50 &&
                   imgBuffer[2] == 0x4E && imgBuffer[3] == 0x47);

  uint8_t* pngBuffer = imgBuffer;
  int      pngBytes  = bytesRead;
  uint8_t* decodedBuffer = nullptr;

  if (!isRawPNG) {
    Serial.println("Raw PNG magic not found, trying base64 decode...");
    // Strip trailing whitespace/newlines from base64 string
    while (pngBytes > 0 && (imgBuffer[pngBytes - 1] == '\n' ||
                              imgBuffer[pngBytes - 1] == '\r' ||
                              imgBuffer[pngBytes - 1] == ' '))
      pngBytes--;

    unsigned int decodedLen = decode_base64_length(imgBuffer, pngBytes);
    decodedBuffer = (uint8_t*)malloc(decodedLen);
    if (!decodedBuffer) {
      Serial.println("malloc failed for base64 decode");
      free(imgBuffer);
      sink.buf = nullptr;
      return false;
    }
    decode_base64(imgBuffer, pngBytes, decodedBuffer);
    pngBuffer = decodedBuffer;
    pngBytes  = (int)decodedLen;
    Serial.printf("Base64 decoded: %d bytes\n", pngBytes);
  }

  int16_t rc = png.openRAM(pngBuffer, pngBytes, PNGDraw);
  if (rc != PNG_SUCCESS) {
    Serial.printf("PNG open error: %d\n", rc);
    free(decodedBuffer);
    free(imgBuffer);
    sink.buf = nullptr;  // Prevent double-free in destructor
    return false;
  }

  epd.fillScreen(TFT_WHITE);
  regionOffsetX = 0;
  regionOffsetY = 0;
  rc = png.decode(NULL, 0);
  png.close();
  free(decodedBuffer);
  free(imgBuffer);
  sink.buf = nullptr;  // Prevent double-free in destructor

  if (rc != PNG_SUCCESS) {
    Serial.printf("PNG decode error: %d\n", rc);
    return false;
  }

  epd.update();
  return true;
}

void fetchAndDisplayImage(bool isColdBoot) {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi not connected, skipping fetch"); return; }

  bool shouldFullFetch = false;

  if (isColdBoot) {
    Serial.println("Cold boot - fetching full image");
    shouldFullFetch = true;
    wakeCounter = 0;
    partialUpdateCount = 0;
  } else {
    wakeCounter++;
    saveWakeCounterToEEPROM(wakeCounter);
    Serial.printf("Wake #%u, full fetch every %u\n", wakeCounter, FULL_FETCH_WAKE_COUNT);
    
    if (wakeCounter >= FULL_FETCH_WAKE_COUNT) {
      Serial.println("Full fetch interval reached");
      shouldFullFetch = true;
      wakeCounter = 0;
    } else {
      Serial.println("Fetching partial updates via imageDiff");
      ImageDiffResult diff = fetchImageDiff(storedChecksum);
      
      if (diff.changeCount == 0) {
        Serial.println("No changes detected");
        return;
      }
      
      bool success = fetchPartialImage(diff);
      
      if (success) {
        storedChecksum = diff.currentChecksum;
        previousChecksum = diff.previousChecksum;
        saveChecksumToEEPROM(storedChecksum);
        hasValidImage = true;
      }
      return;
    }
  }

  if (shouldFullFetch) {
    Serial.println("Fetching full image...");
    bool success = fetchRawImageAndDisplay();
    
    if (success) {
      ImageDiffResult diff = fetchImageDiff(storedChecksum);
      storedChecksum = diff.currentChecksum;
      previousChecksum = diff.previousChecksum;
      saveChecksumToEEPROM(storedChecksum);
      saveWakeCounterToEEPROM(wakeCounter);
      hasValidImage = true;
      partialUpdateCount = 0;
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
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
  esp_deep_sleep_start();
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
