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
#define MAX_IMAGE_WIDTH 800
#include <PNGdec.h>

#include "config.h"
#include "secrets.h"

EPaper epd = EPaper();
PNG png;

// const int BUTTON_D1 = D1;
// const int BUTTON_D2 = D2;
// const int BUTTON_D4 = D4;

String storedChecksum = "";
bool hasValidImage = false;

const uint64_t SLEEP_DURATION_US = (uint64_t)REFRESH_INTERVAL_MINUTES * 60ULL * 1000000ULL;
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

int PNGDraw(PNGDRAW* pDraw) {
  uint16_t lineBuffer[MAX_IMAGE_WIDTH];
  png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_BIG_ENDIAN, 0xFFFFFFFF);
  for (int x = 0; x < pDraw->iWidth; x++) {
    uint16_t p = lineBuffer[x];
    // Extract 8-bit channels from RGB565 and compute luminance
    uint8_t r = (p >> 8) & 0xF8;
    uint8_t g = (p >> 3) & 0xFC;
    uint8_t b = (p << 3) & 0xF8;
    uint8_t lum = (r * 77 + g * 150 + b * 29) >> 8;
    epd.drawPixel(x, pDraw->y, lum >= 128 ? TFT_WHITE : TFT_BLACK);
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
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    digitalWrite(LED_BUILTIN, HIGH);
  } else {
    Serial.println("");
    Serial.println("WiFi connection failed!");
    digitalWrite(LED_BUILTIN, LOW);
  }
}

void reconnectWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, reconnecting...");
    digitalWrite(LED_BUILTIN, LOW);
    connectWiFi();
  }
}

String getBasicAuthHeader() {
  String auth = String(BASIC_AUTH_USER) + ":" + String(BASIC_AUTH_PASS);
  unsigned int encodedLen = encode_base64_length(auth.length());
  unsigned char encoded[encodedLen + 1];
  encode_base64((unsigned char*)auth.c_str(), auth.length(), encoded);
  encoded[encodedLen] = '\0';
  return String("Basic ") + String((char*)encoded);
}

bool fetchChecksumFromAPI(String& checksum) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, API_URL);
  http.addHeader("Authorization", getBasicAuthHeader());
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP error: %d\n", code);
    http.end();
    return false;
  }
  StaticJsonDocument<8> filter;
  filter["checksum"] = true;
  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, *http.getStreamPtr(), DeserializationOption::Filter(filter));
  http.end();
  if (err || !doc["checksum"]) {
    Serial.printf("Checksum parse error: %s\n", err ? err.c_str() : "missing field");
    return false;
  }
  checksum = doc["checksum"].as<String>();
  return true;
}

bool fetchImageFromAPI(String& imageData) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, API_URL);
  http.addHeader("Authorization", getBasicAuthHeader());
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP error: %d\n", code);
    http.end();
    return false;
  }
  StaticJsonDocument<8> filter;
  filter["image"] = true;
  DynamicJsonDocument doc(90000);
  DeserializationError err = deserializeJson(doc, *http.getStreamPtr(), DeserializationOption::Filter(filter));
  http.end();
  if (err || !doc["image"]) {
    Serial.printf("Image parse error: %s\n", err ? err.c_str() : "missing field");
    return false;
  }
  imageData = doc["image"].as<String>();
  return true;
}

void saveChecksumToEEPROM(const String& checksum) {
  for (int i = 0; i < 64 && i < (int)checksum.length(); i++) EEPROM.write(i, checksum[i]);
  EEPROM.write(64, '\0');
  EEPROM.commit();
}

String loadChecksumFromEEPROM() {
  String checksum = ""; char c;
  for (int i = 0; i < 64; i++) { c = EEPROM.read(i); if (c == '\0') break; checksum += c; }
  return checksum;
}

bool decodeAndDisplayImage(const String& base64Image) {
  unsigned int decodedLen = decode_base64_length((unsigned char*)base64Image.c_str(), base64Image.length());
  uint8_t* decodedBuffer = (uint8_t*)malloc(decodedLen);
  if (!decodedBuffer) { Serial.println("Failed to allocate memory"); return false; }
  decode_base64((unsigned char*)base64Image.c_str(), base64Image.length(), decodedBuffer);
  int16_t rc = png.openRAM(decodedBuffer, decodedLen, PNGDraw);
  if (rc == PNG_SUCCESS) {
    epd.fillScreen(TFT_WHITE);
    rc = png.decode(NULL, 0);
    png.close();
    free(decodedBuffer);
    if (rc == PNG_SUCCESS) {
      epd.update();
      return true;
    }
    return false;
  } else { free(decodedBuffer); return false; }
}

void fetchAndDisplayImage() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi not connected, skipping fetch"); return; }

  String checksum = "";
  Serial.println("Fetching checksum from API...");
  Serial.println(API_URL);
  if (!fetchChecksumFromAPI(checksum)) return;

  if (checksum == storedChecksum) {
    Serial.println("Image unchanged, skipping render");
    return;
  }

  Serial.println("New image detected, fetching...");
  String imageData = "";
  if (fetchImageFromAPI(imageData)) {
    if (decodeAndDisplayImage(imageData)) {
      storedChecksum = checksum;
      saveChecksumToEEPROM(checksum);
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
  Serial.printf("Deep sleep i %d minut(er)...\n", REFRESH_INTERVAL_MINUTES);
  Serial.flush();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
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

  EEPROM.begin(128);
  storedChecksum = loadChecksumFromEEPROM(); // Ladda sparad checksum

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

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

  if (WiFi.status() == WL_CONNECTED) {
    fetchAndDisplayImage();
  } else {
    Serial.println("WiFi misslyckades, sover ändå.");
  }

  goToSleep();
}

void loop() {
  // Når aldrig hit – deep sleep i setup() startar om enheten vid uppvakning
}
