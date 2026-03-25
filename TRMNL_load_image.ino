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

unsigned long lastFetchTime = 0;
const unsigned long FETCH_INTERVAL = REFRESH_INTERVAL_MINUTES * 60 * 1000;
bool imagePending = true; // fetch image as soon as WiFi connects
static uint8_t pngBuffer[8192];

unsigned long lastStatusUpdate = 0;
const unsigned long STATUS_CHECK_INTERVAL = 2000;

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

bool fetchImageFromAPI(String& imageData, String& checksum) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  Serial.println("Fetching image from API...");
  Serial.println(API_URL);
  http.begin(client, API_URL);
  http.addHeader("Authorization", getBasicAuthHeader());
  http.addHeader("Accept", "application/json");
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    http.end();
    DynamicJsonDocument doc(payload.length() + 256);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) { Serial.print("JSON parse error: "); Serial.println(error.c_str()); return false; }
    if (doc.containsKey("image") && doc.containsKey("checksum")) {
      imageData = doc["image"].as<String>();
      checksum = doc["checksum"].as<String>();
      return true;
    } else { Serial.println("JSON missing image or checksum fields"); return false; }
  } else { Serial.print("HTTP error: "); Serial.println(httpCode); http.end(); return false; }
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
  String imageData = "", checksum = "";
  if (fetchImageFromAPI(imageData, checksum)) {
    if (checksum != storedChecksum) {
      if (decodeAndDisplayImage(imageData)) {
        storedChecksum = checksum; saveChecksumToEEPROM(checksum); hasValidImage = true;
      }
    } else {
      Serial.println("Image unchanged, skipping render");
    }
  }
}

void fetchDirectImageAndDisplay(const char* url) {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi not connected"); return; }
  Serial.println("Fetching direct PNG...");
  Serial.println(url);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP error: %d\n", httpCode);
    http.end();
    return;
  }

  int contentLength = http.getSize();
  Serial.printf("Content-Length: %d bytes\n", contentLength);

  // Allocate buffer – use known size or a generous max for chunked responses
  const int MAX_PNG_BYTES = 80000;
  int bufSize = (contentLength > 0 && contentLength <= MAX_PNG_BYTES) ? contentLength : MAX_PNG_BYTES;
  uint8_t* buffer = (uint8_t*)malloc(bufSize);
  if (!buffer) { Serial.println("Failed to allocate memory"); http.end(); return; }

  WiFiClient* stream = http.getStreamPtr();
  int totalRead = 0;
  unsigned long activityTimeout = millis() + 10000;
  while ((http.connected() || stream->available()) && totalRead < bufSize && millis() < activityTimeout) {
    int avail = stream->available();
    if (avail > 0) {
      totalRead += stream->readBytes(buffer + totalRead, min(avail, bufSize - totalRead));
      activityTimeout = millis() + 3000; // reset on activity
    }
    delay(1);
  }
  http.end();

  if (totalRead == 0) {
    Serial.println("No data received");
    free(buffer);
    return;
  }
  Serial.printf("Downloaded %d bytes, decoding PNG...\n", totalRead);

  int16_t rc = png.openRAM(buffer, totalRead, PNGDraw);
  if (rc == PNG_SUCCESS) {
    epd.fillScreen(TFT_WHITE);
    rc = png.decode(NULL, 0);
    png.close();
    free(buffer);
    if (rc == PNG_SUCCESS) {
      epd.update();
      Serial.println("Image rendered!");
    } else {
      Serial.printf("PNG decode error: %d\n", rc);
    }
  } else {
    Serial.printf("PNG open error: %d\n", rc);
    free(buffer);
  }
}


void drawTestPattern() {
  Serial.println("Drawing test pattern...");
  epd.fillScreen(TFT_WHITE);
  epd.fillRect(0,   0,   800, 10,  TFT_BLACK); // top bar
  epd.fillRect(0,   470, 800, 10,  TFT_BLACK); // bottom bar
  epd.fillRect(0,   0,   10,  480, TFT_BLACK); // left bar
  epd.fillRect(790, 0,   10,  480, TFT_BLACK); // right bar
  epd.fillRect(350, 215, 100, 50,  TFT_BLACK); // center box
  epd.setTextColor(TFT_BLACK);
  epd.setTextSize(3);
  epd.drawString("TEST OK", 280, 100);
  epd.update();
  Serial.println("Test pattern sent to display");
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

void setup() {
  Serial.begin(115200);
  while (!Serial) { }
  
  Serial.println("TRMNL WiFi Diagnostik");
  Serial.println("=====================");
  
  EEPROM.begin(128);
  storedChecksum = loadChecksumFromEEPROM();
  
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  
  // pinMode(BUTTON_D1, INPUT_PULLUP);
  // pinMode(BUTTON_D2, INPUT_PULLUP);
  // pinMode(BUTTON_D4, INPUT_PULLUP);
  
  epd.init();
  epd.setRotation(0);
  epd.setTextColor(TFT_BLACK);
  
  drawInitialScreen();
  
  connectWiFi();
  
  drawWiFiStatusScreen();
  delay(5000);
  if (WiFi.status() == WL_CONNECTED) {
    fetchAndDisplayImage();
    lastFetchTime = millis();
    imagePending = false;
  }
  
  lastStatusUpdate = millis();
}

void loop() {
  reconnectWiFi();
  
  unsigned long now = millis();
  if (now - lastStatusUpdate >= STATUS_CHECK_INTERVAL) {
    drawWiFiStatusScreen();
    lastStatusUpdate = now;
  }

  // Fetch image immediately once WiFi connects, then every FETCH_INTERVAL
  if (WiFi.status() == WL_CONNECTED) {
    if (imagePending || now - lastFetchTime >= FETCH_INTERVAL) {
      fetchAndDisplayImage();
      lastFetchTime = now;
      imagePending = false;
    }
  }
  
  delay(100);
}
