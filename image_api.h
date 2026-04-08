#ifndef IMAGE_API_H
#define IMAGE_API_H

#include <Arduino.h>

#include "firmware_state.h"

class EPaper;

ImageDiffResult fetchImageDiff();
bool fetchRawImageAndDisplay(EPaper& epd);
void fetchAndDisplayImage(EPaper& epd, FirmwareState& state, bool isColdBoot);

#endif