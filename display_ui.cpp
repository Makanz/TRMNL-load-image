#include "display_ui.h"

#include "driver.h"
#include <TFT_eSPI.h>
#include <WiFi.h>

#include "config.h"
#include "secrets.h"
#include "wifi_manager.h"

namespace {

constexpr int ROW_TITLE = 20;
constexpr int ROW_SSID = 66;
constexpr int ROW_STATUS = 102;
constexpr int ROW_IP = 138;
constexpr int ROW_SIGNAL = 174;
constexpr int ROW_BSSID = 210;
constexpr int ROW_CHANNEL = 246;
constexpr int ROW_HEIGHT = 36;
constexpr int FULL_REFRESH_EVERY = 20;
bool displayInitialized = false;

bool wifiStatusChanged(const FirmwareState& state, const WiFiStatusSnapshot& snapshot) {
  return (snapshot.status != state.wifiDisplay.prevWifiStatus) ||
         (snapshot.ip != state.wifiDisplay.prevIP) ||
         (abs(snapshot.rssi - state.wifiDisplay.prevRSSI) >= 3) ||
         (snapshot.channel != state.wifiDisplay.prevChannel) ||
         (snapshot.bssid != state.wifiDisplay.prevBSSID);
}

void updateRow(EPaper& epd, FirmwareState& state, int y, const String& text, int textSize = 2) {
  epd.fillRect(0, y, SCREEN_WIDTH, ROW_HEIGHT, TFT_WHITE);
  epd.updataPartial(0, y, SCREEN_WIDTH, ROW_HEIGHT);

  if (text.length() > 0) {
    epd.setTextColor(TFT_BLACK);
    epd.setTextSize(textSize);
    epd.drawString(text, 20, y);
    epd.updataPartial(0, y, SCREEN_WIDTH, ROW_HEIGHT);
  }

  state.wifiDisplay.partialUpdateCount++;
  if (state.wifiDisplay.partialUpdateCount >= FULL_REFRESH_EVERY) {
    state.wifiDisplay.isFirstDraw = true;
    state.wifiDisplay.partialUpdateCount = 0;
  }
}

}  // namespace

void ensureDisplayReady(EPaper& epd) {
  if (displayInitialized) {
    return;
  }

  epd.init();
  epd.setRotation(0);
  epd.setTextColor(TFT_BLACK);
  displayInitialized = true;
}

void drawInitialScreen(EPaper& epd) {
  ensureDisplayReady(epd);
  epd.fillScreen(TFT_WHITE);
  epd.setTextColor(TFT_BLACK);
  epd.setTextSize(2);
  epd.drawString("Connecting to WiFi...", 20, 20);
  epd.update();
}

void drawErrorScreen(EPaper& epd, const String& errorMsg) {
  ensureDisplayReady(epd);
  epd.fillScreen(TFT_WHITE);
  epd.setTextColor(TFT_BLACK);
  epd.setTextSize(2);
  epd.drawString("Error:", 20, 20);
  epd.drawString(errorMsg, 20, 50);
  epd.update();
}

void drawWiFiStatusScreen(EPaper& epd, FirmwareState& state) {
  ensureDisplayReady(epd);
  WiFiStatusSnapshot snapshot = readWiFiStatusSnapshot();
  if (!wifiStatusChanged(state, snapshot) && !state.wifiDisplay.isFirstDraw) {
    return;
  }

  String statusStr;
  if (snapshot.status == static_cast<int8_t>(WL_CONNECTED)) statusStr = "Connected";
  else if (snapshot.status == static_cast<int8_t>(WL_DISCONNECTED)) statusStr = "Disconnected";
  else if (snapshot.status == static_cast<int8_t>(WL_CONNECT_FAILED)) statusStr = "Connect failed";
  else if (snapshot.status == static_cast<int8_t>(WL_NO_SSID_AVAIL)) statusStr = "SSID not found";
  else statusStr = "Connecting...";

  if (state.wifiDisplay.isFirstDraw) {
    epd.fillScreen(TFT_WHITE);
    epd.setTextColor(TFT_BLACK);
    epd.setTextSize(3);
    epd.drawString("WiFi Status", 20, ROW_TITLE);
    epd.setTextSize(2);
    epd.drawString("SSID: " + String(WIFI_SSID), 20, ROW_SSID);
    epd.drawString("Status: " + statusStr, 20, ROW_STATUS);
    if (snapshot.status == static_cast<int8_t>(WL_CONNECTED)) {
      epd.drawString("IP: " + snapshot.ip, 20, ROW_IP);
      epd.drawString("Signal: " + String(snapshot.rssi) + " dBm (" + rssiQuality(snapshot.rssi) + ")", 20, ROW_SIGNAL);
      epd.drawString("BSSID: " + snapshot.bssid, 20, ROW_BSSID);
      epd.drawString("Channel: " + String(snapshot.channel), 20, ROW_CHANNEL);
    }
    epd.update();
    state.wifiDisplay.isFirstDraw = false;
  } else {
    if (snapshot.status != state.wifiDisplay.prevWifiStatus) {
      updateRow(epd, state, ROW_STATUS, "Status: " + statusStr);
    }
    if (snapshot.ip != state.wifiDisplay.prevIP) {
      updateRow(epd, state, ROW_IP, snapshot.ip.length() > 0 ? "IP: " + snapshot.ip : "");
    }
    if (abs(snapshot.rssi - state.wifiDisplay.prevRSSI) >= 3) {
      updateRow(epd, state, ROW_SIGNAL, snapshot.rssi != 0 ? "Signal: " + String(snapshot.rssi) + " dBm (" + rssiQuality(snapshot.rssi) + ")" : "");
    }
    if (snapshot.bssid != state.wifiDisplay.prevBSSID) {
      updateRow(epd, state, ROW_BSSID, snapshot.bssid.length() > 0 ? "BSSID: " + snapshot.bssid : "");
    }
    if (snapshot.channel != state.wifiDisplay.prevChannel) {
      updateRow(epd, state, ROW_CHANNEL, snapshot.channel != 0 ? "Channel: " + String(snapshot.channel) : "");
    }
  }

  state.wifiDisplay.prevWifiStatus = snapshot.status;
  state.wifiDisplay.prevIP = snapshot.ip;
  state.wifiDisplay.prevRSSI = snapshot.rssi;
  state.wifiDisplay.prevChannel = snapshot.channel;
  state.wifiDisplay.prevBSSID = snapshot.bssid;
}
