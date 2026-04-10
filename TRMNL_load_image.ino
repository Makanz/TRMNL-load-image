#include <Arduino.h>
#include "driver.h"
#include <TFT_eSPI.h>
#include <EEPROM.h>
#include <esp_sleep.h>
#include <WiFi.h>

#include "config.h"
#include "display_ui.h"
#include "firmware_state.h"
#include "image_api.h"
#include "storage.h"
#include "wifi_manager.h"

EPaper epd = EPaper();
FirmwareState firmwareState;

const int BUTTON_1 = D1;

void goToSleep() {
  uint32_t intervalSeconds = firmwareState.refreshIntervalSeconds;
  if (intervalSeconds == 0 || !isRefreshIntervalValid(intervalSeconds)) {
    intervalSeconds = getDefaultRefreshInterval();
    Serial.printf("Using default refresh interval: %u seconds\n", intervalSeconds);
  } else {
    Serial.printf("Using refresh interval from EEPROM/API: %u seconds\n", intervalSeconds);
  }

  uint64_t sleepDurationUs = (uint64_t)intervalSeconds * 1000000ULL;
  uint32_t minutes = intervalSeconds / 60;
  Serial.printf("Entering deep sleep for %u seconds (%u minutes)...\n", intervalSeconds, minutes);
  Serial.flush();

  esp_sleep_enable_timer_wakeup(sleepDurationUs);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  // Wait up to 3s for Serial - won't block standalone device
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}

  bool isTimerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;

  Serial.println(isTimerWake ? "Woke from deep sleep" : "TRMNL Startup");
  Serial.println("=====================");

  epd.init();
  epd.setRotation(0);
  epd.setTextColor(TFT_BLACK);

  if (!EEPROM.begin(EEPROM_SIZE)) {
    Serial.println("EEPROM.begin failed!");
    drawErrorScreen(epd, "EEPROM error!");
    delay(5000);
  }
  loadPersistedState(firmwareState);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  pinMode(BUTTON_1, INPUT_PULLUP);
  if (digitalRead(BUTTON_1) == LOW) {
    Serial.println("Button 1 pressed – clearing stored checksum");
    clearPersistedImageState(firmwareState);
  }

  if (!isTimerWake) {
    // Only on cold boot: show "Connecting..." screen and WiFi status
    drawInitialScreen(epd);
  }

  connectWiFi();

  if (!isTimerWake) {
    drawWiFiStatusScreen(epd, firmwareState);
    delay(3000);
  }

  Serial.printf("[Heap] After WiFi: %u bytes free\n", ESP.getFreeHeap());

  if (WiFi.status() == WL_CONNECTED) {
    fetchAndDisplayImage(epd, firmwareState, !isTimerWake);
  } else {
    Serial.println("WiFi failed, sleeping anyway.");
  }

  goToSleep();
}

void loop() {
  // Never reached - deep sleep in setup() restarts device on wake
}
