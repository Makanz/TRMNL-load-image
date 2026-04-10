#include "image_api.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <base64.hpp>

#include "bmp_decode.h"
#include "config.h"
#include "driver.h"
#include "secrets.h"
#include "storage.h"
#include <TFT_eSPI.h>

namespace {

const uint32_t FULL_FETCH_WAKE_COUNT = FULL_FETCH_INTERVAL_HOURS * 60 / REFRESH_INTERVAL_MINUTES;

String getBasicAuthHeader() {
  static String cached;
  if (cached.length() > 0) {
    return cached;
  }

  String auth = String(BASIC_AUTH_USER) + ":" + String(BASIC_AUTH_PASS);
  unsigned int encodedLen = encode_base64_length(auth.length());
  unsigned char encoded[encodedLen + 1];
  encode_base64((unsigned char*)auth.c_str(), auth.length(), encoded);
  encoded[encodedLen] = '\0';
  cached = String("Basic ") + String((char*)encoded);
  return cached;
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
  Serial.printf("[Heap] Before checksum-fetch: %u bytes free\n", ESP.getFreeHeap());

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Checksum HTTP error: %d\n", code);
    http.end();
    return "";
  }

  String payload = http.getString();
  http.end();
  Serial.printf("[Heap] After checksum-fetch: %u bytes free, payload: %d bytes\n", ESP.getFreeHeap(), payload.length());
  Serial.printf("Checksum raw response: %s\n", payload.c_str());

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("Checksum JSON error: %s\n", err.c_str());
    return "";
  }

  return doc["checksum"] | "";
}

}  // namespace

ImageDiffResult fetchImageDiff() {
  ImageDiffResult result;

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

  if (root.containsKey("refreshInterval")) {
    uint32_t newInterval = root["refreshInterval"].as<uint32_t>();
    if (isRefreshIntervalValid(newInterval)) {
      Serial.printf("Refresh interval from API: %u seconds\n", newInterval);
      saveRefreshIntervalToEEPROM(newInterval);
      result.refreshIntervalSeconds = newInterval;
    } else {
      Serial.printf("Invalid refreshInterval from API: %u (range: %u-%u)\n",
                    newInterval, REFRESH_INTERVAL_MIN_SECONDS, REFRESH_INTERVAL_MAX_SECONDS);
    }
  } else {
    Serial.println("No refreshInterval in imageDiff response");
  }

  Serial.printf("Current checksum: %s\n", result.currentChecksum.c_str());
  Serial.printf("Previous checksum: %s\n", result.previousChecksum.c_str());

  if (changes.isNull()) {
    Serial.println("No changes array in response");
    Serial.printf("Found 0 changes, current: %s\n", result.currentChecksum.c_str());
    return result;
  }

  result.changeCount = 0;
  for (JsonObject change : changes) {
    if (result.changeCount >= MAX_IMAGE_CHANGES) {
      break;
    }
    result.changes[result.changeCount].x = change["x"] | 0;
    result.changes[result.changeCount].y = change["y"] | 0;
    result.changes[result.changeCount].width = change["width"] | 0;
    result.changes[result.changeCount].height = change["height"] | 0;
    result.changeCount++;
  }

  Serial.printf("Found %d changes, current: %s\n", result.changeCount, result.currentChecksum.c_str());
  return result;
}

bool fetchRawImageAndDisplay(EPaper& epd) {
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  http.begin(client, API_URL_IMAGE);
  http.setTimeout(15000);
  http.addHeader("Authorization", getBasicAuthHeader());

  Serial.println("Fetching raw image...");
  Serial.printf("[Heap] Before image-fetch: %u bytes free\n", ESP.getFreeHeap());

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Image HTTP error: %d\n", code);
    http.end();
    return false;
  }

  BMPDecodeStream decoder(epd);
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

void fetchAndDisplayImage(EPaper& epd, FirmwareState& state, bool isColdBoot) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skipping fetch");
    return;
  }

  bool shouldFullFetch = false;

  if (isColdBoot) {
    Serial.println("Cold boot - fetching full image");
    shouldFullFetch = true;
    state.wakeCounter = 0;
  } else {
    state.wakeCounter++;
    saveWakeCounterToEEPROM(state.wakeCounter);
    Serial.printf("Wake #%u, full fetch every %u\n", state.wakeCounter, FULL_FETCH_WAKE_COUNT);

    if (state.wakeCounter >= FULL_FETCH_WAKE_COUNT) {
      Serial.println("Full fetch interval reached");
      shouldFullFetch = true;
      state.wakeCounter = 0;
    } else {
      Serial.println("Checking for changes...");
      ImageDiffResult diff = fetchImageDiff();
      if (diff.refreshIntervalSeconds > 0) {
        state.refreshIntervalSeconds = diff.refreshIntervalSeconds;
        saveRefreshIntervalToEEPROM(diff.refreshIntervalSeconds);
        Serial.printf("Updated refresh interval to %u seconds\n", diff.refreshIntervalSeconds);
      }
      if (!diff.currentChecksum.isEmpty() && diff.currentChecksum == state.storedChecksum) {
        Serial.println("No changes detected");
        return;
      }
      shouldFullFetch = true;
    }
  }

  if (shouldFullFetch) {
    Serial.println("Fetching full image...");
    bool success = fetchRawImageAndDisplay(epd);
    if (success) {
      ImageDiffResult diff = fetchImageDiff();
      state.storedChecksum = diff.currentChecksum;
      state.previousChecksum = diff.previousChecksum;
      if (diff.refreshIntervalSeconds > 0) {
        state.refreshIntervalSeconds = diff.refreshIntervalSeconds;
        saveRefreshIntervalToEEPROM(diff.refreshIntervalSeconds);
        Serial.printf("Updated refresh interval to %u seconds\n", diff.refreshIntervalSeconds);
      }
      saveChecksumToEEPROM(state.storedChecksum);
      saveWakeCounterToEEPROM(state.wakeCounter);
      state.hasValidImage = true;
    }
  }
}
