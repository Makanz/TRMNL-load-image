#ifndef DISPLAY_UI_H
#define DISPLAY_UI_H

#include <Arduino.h>

#include "firmware_state.h"

class EPaper;

void drawInitialScreen(EPaper& epd);
void drawErrorScreen(EPaper& epd, const String& errorMsg);
void drawWiFiStatusScreen(EPaper& epd, FirmwareState& state);

#endif