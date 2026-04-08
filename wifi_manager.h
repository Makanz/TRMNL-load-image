#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "firmware_state.h"

String rssiQuality(int rssi);
WiFiStatusSnapshot readWiFiStatusSnapshot();
void connectWiFi();
void reconnectWiFi();

#endif
