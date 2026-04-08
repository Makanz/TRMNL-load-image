#ifndef IMAGE_API_H
#define IMAGE_API_H

#include <Arduino.h>

#include "driver.h"
#include <TFT_eSPI.h>

#include "firmware_state.h"

String getBasicAuthHeader();
String fetchChecksum();
ImageDiffResult fetchImageDiff();
bool fetchRawImageAndDisplay(EPaper& epd);
void fetchAndDisplayImage(EPaper& epd, FirmwareState& state, bool isColdBoot);

#endif
