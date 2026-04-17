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
  http.setTimeout(HTTP_TIMEOUT_META_MS);
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
  
  if (payload.isEmpty()) {
    Serial.println("Checksum response is empty");
    return "";
  }
  
  Serial.printf("[Heap] After checksum-fetch: %u bytes free, payload: %d bytes\n", ESP.getFreeHeap(), payload.length());
  Serial.printf("Checksum raw response: %s\n", payload.c_str());

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("Checksum JSON error: %s\n", err.c_str());
    return "";
  }

  String checksum = doc["checksum"] | "";
  if (checksum.isEmpty()) {
    Serial.println("No checksum value in response");
  }
  return checksum;
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
  http.setTimeout(HTTP_TIMEOUT_META_MS);
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
  http.setTimeout(HTTP_TIMEOUT_IMAGE_MS);
  http.addHeader("Authorization", getBasicAuthHeader());

  Serial.println("Fetching raw image...");
  Serial.printf("[Heap] Before image-fetch: %u bytes free\n", ESP.getFreeHeap());

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Image HTTP error: %d\n", code);
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    Serial.println("Image response has no or invalid content length");
    http.end();
    return false;
  }
  Serial.printf("Image content length: %d bytes\n", contentLength);

  BMPDecodeStream decoder(epd);
  http.writeToStream(&decoder);
  http.end();

  if (!decoder.valid()) {
    Serial.println("BMP decode failed - invalid header or dimensions");
    return false;
  }

  int rowsDone = decoder.rowsDone();
  Serial.printf("Drew %d rows\n", rowsDone);
  
  if (rowsDone <= 0) {
    Serial.println("BMP decode failed - no rows rendered");
    return false;
  }
  
  Serial.println("Calling epd.update()...");
  epd.update();
  Serial.println("BMP rendered successfully");
  return true;
}

ImageFetchResult fetchImageWithRetry(EPaper& epd, FirmwareState& state, bool isColdBoot) {
  const uint32_t BACKOFF_MS[] = {0, 1000, 2000};
  const int MAX_RETRIES = 3;

  for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      Serial.printf("Retry attempt %d/%d, waiting %u ms...\n", attempt, MAX_RETRIES - 1, BACKOFF_MS[attempt]);
      delay(BACKOFF_MS[attempt]);
    }

    Serial.printf("Image fetch attempt %d/%d\n", attempt + 1, MAX_RETRIES);
    
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected");
      ImageFetchResult result;
      result.success = false;
      result.errorCode = ErrorCode::WIFI_CONNECT_FAILED;
      return result;
    }

    // Attempt the original fetchAndDisplayImage logic inline
    bool shouldFullFetch = false;
    bool intervalReached = false;
    bool attemptFailed = false;
    ErrorCode errorCode = ErrorCode::NONE;

    if (isColdBoot) {
      Serial.println("Cold boot - fetching full image");
      shouldFullFetch = true;
    } else {
      uint32_t sleptIntervalSeconds = (state.refreshIntervalSeconds > 0)
          ? state.refreshIntervalSeconds
          : getDefaultRefreshInterval();
      uint32_t fullFetchIntervalSeconds = getFullFetchIntervalSeconds();

      state.wakeCounter++;
      uint64_t elapsedSinceLastFullFetch =
          (uint64_t)state.elapsedFullFetchSeconds + (uint64_t)sleptIntervalSeconds;
      if (elapsedSinceLastFullFetch > fullFetchIntervalSeconds) {
        elapsedSinceLastFullFetch = fullFetchIntervalSeconds;
      }
      state.elapsedFullFetchSeconds = (uint32_t)elapsedSinceLastFullFetch;

      saveWakeCounterToEEPROM(state.wakeCounter);
      saveElapsedFullFetchSecondsToEEPROM(state.elapsedFullFetchSeconds);
      Serial.printf("Wake #%u, slept %u sec, elapsed since last full fetch %u/%u sec\n",
                    state.wakeCounter,
                    sleptIntervalSeconds,
                    state.elapsedFullFetchSeconds,
                    fullFetchIntervalSeconds);

      if (state.elapsedFullFetchSeconds >= fullFetchIntervalSeconds) {
        Serial.println("Full fetch interval reached");
        shouldFullFetch = true;
        intervalReached = true;
      } else {
        Serial.println("Checking for changes...");
        ImageDiffResult diff = fetchImageDiff();
        if (diff.currentChecksum.isEmpty() && diff.previousChecksum.isEmpty()) {
          Serial.println("Failed to fetch diff");
          attemptFailed = true;
          errorCode = ErrorCode::API_JSON_ERROR;
        } else {
          if (diff.refreshIntervalSeconds > 0) {
            state.refreshIntervalSeconds = diff.refreshIntervalSeconds;
            saveRefreshIntervalToEEPROM(diff.refreshIntervalSeconds);
            Serial.printf("Updated refresh interval to %u seconds\n", diff.refreshIntervalSeconds);
          }
          if (!diff.currentChecksum.isEmpty() && diff.currentChecksum == state.storedChecksum) {
            Serial.println("No changes detected");
            ImageFetchResult result;
            result.success = true;
            result.errorCode = ErrorCode::NONE;
            return result;
          }
          shouldFullFetch = true;
        }
      }
    }

    if (attemptFailed) {
      if (attempt < MAX_RETRIES - 1) {
        saveErrorToEEPROM(errorCode, millis() / 1000);
        continue;
      } else {
        ImageFetchResult result;
        result.success = false;
        result.errorCode = errorCode;
        saveErrorToEEPROM(errorCode, millis() / 1000);
        return result;
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
        state.elapsedFullFetchSeconds = 0;
        state.wakeCounter = 0;
        saveChecksumToEEPROM(state.storedChecksum);
        saveElapsedFullFetchSecondsToEEPROM(state.elapsedFullFetchSeconds);
        saveWakeCounterToEEPROM(state.wakeCounter);
        state.hasValidImage = true;
        Serial.println("Full fetch completed - reset elapsed full-refresh timer");
        ImageFetchResult result;
        result.success = true;
        result.errorCode = ErrorCode::NONE;
        return result;
      } else {
        Serial.println("Full fetch failed - retrying");
        if (attempt < MAX_RETRIES - 1) {
          saveErrorToEEPROM(ErrorCode::INVALID_BMP, millis() / 1000);
          continue;
        } else {
          ImageFetchResult result;
          result.success = false;
          result.errorCode = ErrorCode::INVALID_BMP;
          saveErrorToEEPROM(ErrorCode::INVALID_BMP, millis() / 1000);
          return result;
        }
      }
    }
  }

  ImageFetchResult result;
  result.success = false;
  result.errorCode = ErrorCode::UNKNOWN;
  saveErrorToEEPROM(ErrorCode::UNKNOWN, millis() / 1000);
  return result;
}

void fetchAndDisplayImage(EPaper& epd, FirmwareState& state, bool isColdBoot) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skipping fetch");
    return;
  }

  ImageFetchResult result = fetchImageWithRetry(epd, state, isColdBoot);
  if (!result.success) {
    Serial.printf("Image fetch failed after all retries. Error code: %u\n", static_cast<uint16_t>(result.errorCode));
  }
}
