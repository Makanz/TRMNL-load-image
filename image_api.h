#ifndef IMAGE_API_H
#define IMAGE_API_H

#include <Arduino.h>

#include "firmware_state.h"

class EPaper;

struct ImageFetchResult {
  bool success = false;
  ErrorCode errorCode = ErrorCode::NONE;
};

ImageDiffResult fetchImageDiff();
bool fetchRawImageAndDisplay(EPaper& epd);
ImageFetchResult fetchImageWithRetry(EPaper& epd, FirmwareState& state, bool isColdBoot);
void fetchAndDisplayImage(EPaper& epd, FirmwareState& state, bool isColdBoot);

#endif