#include "wifi_manager.h"

#include <Arduino.h>
#include <WiFi.h>

#include "secrets.h"

String rssiQuality(int rssi) {
  if (rssi >= -50) return "Utmarkt";
  if (rssi >= -65) return "Bra";
  if (rssi >= -75) return "Svag";
  return "Dalig";
}

WiFiStatusSnapshot readWiFiStatusSnapshot() {
  WiFiStatusSnapshot snapshot;

  snapshot.status = WiFi.status();
  snapshot.rssi = (snapshot.status == WL_CONNECTED) ? WiFi.RSSI() : 0;
  snapshot.ip = (snapshot.status == WL_CONNECTED) ? WiFi.localIP().toString() : "";
  snapshot.channel = (snapshot.status == WL_CONNECTED) ? WiFi.channel() : 0;
  snapshot.bssid = (snapshot.status == WL_CONNECTED) ? WiFi.BSSIDstr() : "";

  return snapshot;
}

void connectWiFi() {
  Serial.println("Connecting to WiFi...");

  const int MAX_RETRIES = 5;
  const int POLL_INTERVAL_MS = 500;
  const int POLLS_PER_ATTEMPT = 20;
  const int RETRY_DELAY_MS = 3000;

  WiFi.mode(WIFI_STA);

  for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
    Serial.printf("Attempt %d/%d\n", attempt, MAX_RETRIES);

    WiFi.disconnect(true);
    delay(500);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    for (int poll = 0; poll < POLLS_PER_ATTEMPT; poll++) {
      if (WiFi.status() == WL_CONNECTED) {
        break;
      }
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
