#include "wifi_manager.h"

#include <Arduino.h>
#include <WiFi.h>

#include "firmware_state.h"
#include "secrets.h"
#include "storage.h"

String rssiQuality(int rssi) {
  if (rssi >= -50) return "Excellent";
  if (rssi >= -65) return "Good";
  if (rssi >= -75) return "Weak";
  return "Poor";
}

WiFiStatusSnapshot readWiFiStatusSnapshot() {
  WiFiStatusSnapshot snapshot;

  snapshot.status = static_cast<int8_t>(WiFi.status());
  snapshot.rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  snapshot.ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
  snapshot.channel = (WiFi.status() == WL_CONNECTED) ? WiFi.channel() : 0;
  snapshot.bssid = (WiFi.status() == WL_CONNECTED) ? WiFi.BSSIDstr() : "";

  return snapshot;
}

void connectWiFi() {
  Serial.println("Connecting to WiFi...");

  const int MAX_RETRIES = 5;
  const int POLLS_PER_ATTEMPT = 20;

  WiFi.mode(WIFI_STA);

  for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    Serial.printf("Attempt %d/%d\n", attempt, MAX_RETRIES);

    WiFi.disconnect(true);
    delay(WIFI_DISCONNECT_DELAY_MS);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    for (int poll = 0; poll < POLLS_PER_ATTEMPT; poll++) {
      if (WiFi.status() == WL_CONNECTED) {
        break;
      }
      delay(WIFI_POLL_INTERVAL_MS);
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

    wl_status_t status = WiFi.status();
    Serial.printf("Attempt %d failed (status %d)", attempt, status);
    if (attempt < MAX_RETRIES) {
      Serial.printf(", retrying in %d s...\n", WIFI_RETRY_DELAY_MS / 1000);
      delay(WIFI_RETRY_DELAY_MS);
    } else {
      Serial.println();
      saveErrorToEEPROM(ErrorCode::WIFI_CONNECT_FAILED, millis() / 1000);
    }
  }

  Serial.println("WiFi connection failed!");
  digitalWrite(LED_BUILTIN, LOW);
}