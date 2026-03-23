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
#include <PNGdec.h>

#include "config.h"
#include "secrets.h"

EPaper epd = EPaper();
PNG png;

const int BUTTON_D1 = D1;
const int BUTTON_D2 = D2;
const int BUTTON_D4 = D4;

String storedChecksum = "";
bool hasValidImage = false;

unsigned long lastFetchTime = 0;
const unsigned long FETCH_INTERVAL = REFRESH_INTERVAL_MINUTES * 60 * 1000;

#define MAX_IMAGE_WIDTH 800
static uint8_t pngBuffer[8192];

int PNGDraw(PNGDRAW* pDraw) {
  uint16_t lineBuffer[MAX_IMAGE_WIDTH];
  png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFFFF);
  epd.pushImage(0, pDraw->y, pDraw->iWidth, 1, lineBuffer);
  return 0;
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
    
    Serial.println("Response received, parsing JSON...");
    
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
      Serial.print("JSON parse error: ");
      Serial.println(error.c_str());
      return false;
    }
    
    if (doc.containsKey("image") && doc.containsKey("checksum")) {
      imageData = doc["image"].as<String>();
      checksum = doc["checksum"].as<String>();
      
      Serial.print("Checksum: ");
      Serial.println(checksum);
      Serial.print("Image size: ");
      Serial.println(imageData.length());
      
      return true;
    } else {
      Serial.println("JSON missing image or checksum fields");
      return false;
    }
  } else {
    Serial.print("HTTP error: ");
    Serial.println(httpCode);
    http.end();
    return false;
  }
}

void saveChecksumToEEPROM(const String& checksum) {
  for (int i = 0; i < 64 && i < checksum.length(); i++) {
    EEPROM.write(i, checksum[i]);
  }
  EEPROM.write(64, '\0');
  EEPROM.commit();
}

String loadChecksumFromEEPROM() {
  String checksum = "";
  char c;
  for (int i = 0; i < 64; i++) {
    c = EEPROM.read(i);
    if (c == '\0') break;
    checksum += c;
  }
  return checksum;
}

bool decodeAndDisplayImage(const String& base64Image) {
  Serial.println("Decoding base64 image...");
  
  unsigned int decodedLen = decode_base64_length((unsigned char*)base64Image.c_str(), base64Image.length());
  uint8_t* decodedBuffer = (uint8_t*)malloc(decodedLen);
  
  if (!decodedBuffer) {
    Serial.println("Failed to allocate memory for decoded image");
    return false;
  }
  
  decode_base64((unsigned char*)base64Image.c_str(), base64Image.length(), decodedBuffer);
  
  Serial.println("Parsing PNG...");
  
  int16_t rc = png.openRAM(decodedBuffer, decodedLen, PNGDraw);
  if (rc == PNG_SUCCESS) {
    Serial.printf("PNG image: %d x %d\n", png.getWidth(), png.getHeight());
    
    epd.fillScreen(TFT_WHITE);
    
    rc = png.decode(0, 0);
    png.close();
    
    free(decodedBuffer);
    
    if (rc == PNG_SUCCESS) {
      Serial.println("Image displayed successfully");
      return true;
    } else {
      Serial.printf("Failed to decode PNG: %d\n", rc);
      return false;
    }
  } else {
    Serial.printf("Failed to open PNG: %d\n", rc);
    free(decodedBuffer);
    return false;
  }
}

void fetchAndDisplayImage() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skipping fetch");
    return;
  }
  
  String imageData = "";
  String checksum = "";
  
  if (fetchImageFromAPI(imageData, checksum)) {
    Serial.print("Current checksum: ");
    Serial.println(checksum);
    Serial.print("Stored checksum: ");
    Serial.println(storedChecksum);
    
    if (checksum != storedChecksum) {
      Serial.println("Checksum changed, updating display...");
      
      if (decodeAndDisplayImage(imageData)) {
        storedChecksum = checksum;
        saveChecksumToEEPROM(checksum);
        hasValidImage = true;
        Serial.println("Image updated successfully");
      } else {
        Serial.println("Failed to decode/display image");
      }
    } else {
      Serial.println("Checksum unchanged, skipping display update");
    }
  } else {
    Serial.println("Failed to fetch image from API");
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

void setup() {
  Serial.begin(115200);
  while (!Serial) { }
  
  Serial.println("TRMNL E-Paper Image Display");
  Serial.println("============================");
  
  EEPROM.begin(128);
  
  storedChecksum = loadChecksumFromEEPROM();
  Serial.print("Loaded checksum from EEPROM: ");
  Serial.println(storedChecksum);
  
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  
  pinMode(BUTTON_D1, INPUT_PULLUP);
  pinMode(BUTTON_D2, INPUT_PULLUP);
  pinMode(BUTTON_D4, INPUT_PULLUP);
  
  epd.init();
  epd.setRotation(0);
  epd.setTextColor(TFT_BLACK);
  
  drawInitialScreen();
  
  connectWiFi();
  
  fetchAndDisplayImage();
  
  lastFetchTime = millis();
}

void loop() {
  reconnectWiFi();
  
  bool buttonPressed = false;
  
  if (!digitalRead(BUTTON_D1) || !digitalRead(BUTTON_D2) || !digitalRead(BUTTON_D4)) {
    buttonPressed = true;
    Serial.println("Button pressed - refreshing image");
    fetchAndDisplayImage();
    delay(200);
  }
  
  unsigned long currentTime = millis();
  
  if (!buttonPressed && (currentTime - lastFetchTime >= FETCH_INTERVAL)) {
    Serial.println("Timer triggered - fetching new image");
    fetchAndDisplayImage();
    lastFetchTime = currentTime;
  }
  
  delay(100);
}
