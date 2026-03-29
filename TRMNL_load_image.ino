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

#define FRAME_WIDTH  800
#define FRAME_HEIGHT 480
#define FRAME_SIZE   (FRAME_WIDTH * FRAME_HEIGHT / 8)

static uint8_t* prevFrameBuffer = nullptr;
static uint8_t* currFrameBuffer = nullptr;
static bool frameBufferInited = false;

static int changedMinY = FRAME_HEIGHT;
static int changedMaxY = -1;
static bool hasChanges = false;

static int renderOffsetX = 0;
static int renderOffsetY = 0;
static int renderClipW = 0;
static int renderClipH = 0;
static bool useRegionRender = false;

inline void setPixelBit(uint8_t* buf, int x, int y, bool black) {
  int byteIdx = (y * FRAME_WIDTH + x) / 8;
  int bitIdx = 7 - ((y * FRAME_WIDTH + x) % 8);
  if (black) buf[byteIdx] &= ~(1 << bitIdx);
  else      buf[byteIdx] |=  (1 << bitIdx);
}

inline bool getPixelBit(uint8_t* buf, int x, int y) {
  int byteIdx = (y * FRAME_WIDTH + x) / 8;
  int bitIdx = 7 - ((y * FRAME_WIDTH + x) % 8);
  return (buf[byteIdx] >> bitIdx) & 1;
}

void commitCurrentFrame() {
  if (!prevFrameBuffer || !currFrameBuffer) return;
  memcpy(prevFrameBuffer, currFrameBuffer, FRAME_SIZE);
}

void initFrameBuffers() {
  if (frameBufferInited) return;
  prevFrameBuffer = (uint8_t*)malloc(FRAME_SIZE);
  currFrameBuffer = (uint8_t*)malloc(FRAME_SIZE);
  if (prevFrameBuffer && currFrameBuffer) {
    memset(prevFrameBuffer, 0xFF, FRAME_SIZE);
    memset(currFrameBuffer, 0xFF, FRAME_SIZE);
    frameBufferInited = true;
    Serial.println("[FrameBuffer] Init: 94KB allocated");
  } else {
    Serial.println("[FrameBuffer] FAILED to allocate!");
    if (prevFrameBuffer) free(prevFrameBuffer);
    if (currFrameBuffer) free(currFrameBuffer);
    prevFrameBuffer = nullptr;
    currFrameBuffer = nullptr;
  }
}

void freeFrameBuffers() {
  if (prevFrameBuffer) { free(prevFrameBuffer); prevFrameBuffer = nullptr; }
  if (currFrameBuffer) { free(currFrameBuffer); currFrameBuffer = nullptr; }
  frameBufferInited = false;
}

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
  
  int py = pDraw->y;
  if (useRegionRender && py >= renderClipH) return 1;

  int screenX = useRegionRender ? renderOffsetX : 0;
  int screenY = useRegionRender ? renderOffsetY + py : py;
  bool rowHasChanges = false;
  bool useDiff = !useRegionRender && frameBufferInited && prevFrameBuffer && currFrameBuffer;
  
  for (int px = 0; px < pDraw->iWidth; px++) {
    if (useRegionRender && px >= renderClipW) break;
    
    uint16_t p = lineBuffer[px];
    uint8_t r = (p >> 8) & 0xF8;
    uint8_t g = (p >> 3) & 0xFC;
    uint8_t b = (p << 3) & 0xF8;
    uint8_t lum = (r * 77 + g * 150 + b * 29) >> 8;
    bool isBlack = lum < 128;
    
    if (useDiff) {
      bool prevPixel = getPixelBit(prevFrameBuffer, screenX + px, screenY);
      bool same = (isBlack ? 0 : 1) == prevPixel;
      
      if (!same) {
        setPixelBit(currFrameBuffer, screenX + px, screenY, isBlack);
        epd.drawPixel(screenX + px, screenY, isBlack ? TFT_BLACK : TFT_WHITE);
        rowHasChanges = true;
      }
    } else {
      epd.drawPixel(screenX + px, screenY, isBlack ? TFT_BLACK : TFT_WHITE);
    }
  }
  
  if (useDiff && rowHasChanges) {
    hasChanges = true;
    if (screenY < changedMinY) changedMinY = screenY;
    if (screenY > changedMaxY) changedMaxY = screenY;
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
  for (int i = 0; i < 64 && i < (int)checksum.length(); i++) EEPROM.write(i, checksum[i]);
  EEPROM.write(64, '\0');
  EEPROM.commit();
}

String loadChecksumFromEEPROM() {
  String checksum = ""; char c;
  for (int i = 0; i < 64; i++) { c = EEPROM.read(i); if (c == '\0') break; checksum += c; }
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

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("Checksum JSON error: %s\n", err.c_str());
    return "";
  }

  return doc["checksum"] | "";
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
  initFrameBuffers();
  if (!frameBufferInited) {
    Serial.println("[Render] Frame buffers not available, using full render");
  }
  
  hasChanges = false;
  changedMinY = FRAME_HEIGHT;
  changedMaxY = -1;
  
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

  bool isRawPNG = (bytesRead >= 4 &&
                   imgBuffer[0] == 0x89 && imgBuffer[1] == 0x50 &&
                   imgBuffer[2] == 0x4E && imgBuffer[3] == 0x47);

  uint8_t* pngBuffer = imgBuffer;
  int      pngBytes  = bytesRead;
  uint8_t* decodedBuffer = nullptr;

  if (!isRawPNG) {
    Serial.println("Raw PNG magic not found, trying base64 decode...");
    while (pngBytes > 0 && (imgBuffer[pngBytes - 1] == '\n' ||
                              imgBuffer[pngBytes - 1] == '\r' ||
                              imgBuffer[pngBytes - 1] == ' '))
      pngBytes--;

    unsigned int decodedLen = decode_base64_length(imgBuffer, pngBytes);
    decodedBuffer = (uint8_t*)malloc(decodedLen);
    if (!decodedBuffer) {
      Serial.println("malloc failed for base64 decode");
      free(imgBuffer);
      return false;
    }
    decode_base64(imgBuffer, pngBytes, decodedBuffer);
    pngBuffer = decodedBuffer;
    pngBytes  = (int)decodedLen;
    Serial.printf("Base64 decoded: %d bytes\n", pngBytes);
  }

  if (frameBufferInited) {
    epd.fillScreen(TFT_WHITE);
  }
  
  int16_t rc = png.openRAM(pngBuffer, pngBytes, PNGDraw);
  if (rc != PNG_SUCCESS) {
    Serial.printf("PNG open error: %d\n", rc);
    free(decodedBuffer);
    free(imgBuffer);
    return false;
  }

  rc = png.decode(NULL, 0);
  png.close();
  free(decodedBuffer);
  sink.clear();

  if (rc != PNG_SUCCESS) {
    Serial.printf("PNG decode error: %d\n", rc);
    return false;
  }

  if (!frameBufferInited) {
    epd.update();
    return true;
  }

  if (!hasChanges) {
    Serial.println("[Render] No pixels changed, skipping e-ink update");
    return true;
  }

  int regionHeight = changedMaxY - changedMinY + 1;
  Serial.printf("[Render] Changed rows: %d-%d (%d rows)\n", changedMinY, changedMaxY, regionHeight);

  if (isFirstDraw) {
    epd.update();
    isFirstDraw = false;
  } else {
    epd.updataPartial(0, changedMinY, FRAME_WIDTH, regionHeight);
  }

  commitCurrentFrame();
  return true;
}

void fetchAndDisplayImage() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi not connected, skipping fetch"); return; }

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, API_URL_IMAGE_DIFF);
  http.setTimeout(10000);
  http.addHeader("Authorization", getBasicAuthHeader());
  http.addHeader("Accept", "application/json");

  Serial.println("Fetching image diff...");
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("ImageDiff HTTP error: %d\n", code);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();
  Serial.printf("ImageDiff payload: %d bytes\n", payload.length());

  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("ImageDiff JSON error: %s\n", err.c_str());
    return;
  }

  String currentChecksum = doc["currentChecksum"] | "";
  if (currentChecksum.isEmpty()) {
    Serial.println("No currentChecksum in response");
    return;
  }

  if (currentChecksum == storedChecksum) {
    Serial.println("Image unchanged, skipping render");
    return;
  }

  Serial.printf("New image: %s -> %s\n", storedChecksum.c_str(), currentChecksum.c_str());

  JsonArray changes = doc["changes"];
  if (changes.isNull() || changes.size() == 0) {
    Serial.println("No changes in diff");
    storedChecksum = currentChecksum;
    saveChecksumToEEPROM(currentChecksum);
    return;
  }

  Serial.printf("Processing %d changed regions...\n", changes.size());

  for (JsonObject change : changes) {
    int x = change["x"] | 0;
    int y = change["y"] | 0;
    int w = change["width"] | 1;
    int h = change["height"] | 1;
    Serial.printf("  Region: x=%d, y=%d, w=%d, h=%d\n", x, y, w, h);

    String regionUrl = String(API_URL_REGION) + "&x=" + x + "&y=" + y + "&w=" + w + "&h=" + h;
    HTTPClient httpRegion;
    httpRegion.begin(client, regionUrl);
    httpRegion.setTimeout(15000);
    httpRegion.addHeader("Authorization", getBasicAuthHeader());

    int regionCode = httpRegion.GET();
    if (regionCode != HTTP_CODE_OK) {
      Serial.printf("  Region fetch error: %d\n", regionCode);
      httpRegion.end();
      continue;
    }

    String regionData = httpRegion.getString();
    httpRegion.end();

    uint8_t* decoded = nullptr;
    int decodedLen = 0;

    if (regionData.length() >= 4 &&
        (uint8_t)regionData[0] == 0x89 && (uint8_t)regionData[1] == 0x50) {
      decoded = (uint8_t*)malloc(regionData.length());
      if (decoded) {
        memcpy(decoded, regionData.c_str(), regionData.length());
        decodedLen = regionData.length();
      }
    } else {
      unsigned char* inputBuf = (unsigned char*)regionData.c_str();
      unsigned int decLen = decode_base64_length(inputBuf, regionData.length());
      decoded = (uint8_t*)malloc(decLen);
      if (decoded) {
        decodedLen = decode_base64(inputBuf, regionData.length(), decoded);
      }
    }

    if (!decoded || decodedLen == 0) {
      Serial.println("  Failed to decode region data");
      if (decoded) free(decoded);
      continue;
    }

    epd.fillRect(x, y, w, h, TFT_WHITE);
    epd.updataPartial(x, y, w, h);

    useRegionRender = true;
    renderOffsetX = x;
    renderOffsetY = y;
    renderClipW = w;
    renderClipH = h;

    int16_t rc = png.openRAM(decoded, decodedLen, PNGDraw);

    if (rc == PNG_SUCCESS) {
      png.decode(NULL, 0);
      epd.updataPartial(x, y, w, h);
      Serial.printf("  Rendered region %dx%d at (%d,%d)\n", w, h, x, y);
    } else {
      Serial.printf("  PNG decode error: %d\n", rc);
    }

    png.close();
    useRegionRender = false;
    free(decoded);
  }

  storedChecksum = currentChecksum;
  saveChecksumToEEPROM(currentChecksum);
  hasValidImage = true;
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

  EEPROM.begin(128);
  storedChecksum = loadChecksumFromEEPROM(); // Ladda sparad checksum

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  pinMode(BUTTON_1, INPUT_PULLUP);
  if (digitalRead(BUTTON_1) == LOW) {
    Serial.println("Button 1 pressed – clearing stored checksum");
    storedChecksum = "";
    clearChecksumInEEPROM();
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
    fetchAndDisplayImage();
  } else {
    Serial.println("WiFi misslyckades, sover ändå.");
  }

  goToSleep();
}

void loop() {
  // Når aldrig hit – deep sleep i setup() startar om enheten vid uppvakning
}
