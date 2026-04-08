#include "bmp_decode.h"

#include "driver.h"
#include <TFT_eSPI.h>

#include "config.h"

static_assert(SCREEN_WIDTH <= 800, "row_ buffer too small for SCREEN_WIDTH");

BMPDecodeStream::BMPDecodeStream(EPaper& epd) : epd_(epd) {}

bool BMPDecodeStream::valid() const {
  return valid_;
}

int BMPDecodeStream::rowsDone() const {
  return rowsDone_;
}

size_t BMPDecodeStream::write(const uint8_t* data, size_t sz) {
  for (size_t i = 0; i < sz; i++) {
    uint8_t value = data[i];

    if (headerPos_ < sizeof(header_)) {
      header_[headerPos_++] = value;
      if (headerPos_ == sizeof(header_)) {
        if (header_[0] != 0x42 || header_[1] != 0x4D) {
          Serial.println("BMP magic mismatch");
          return sz;
        }

        pixelOffset_ = header_[10] | (header_[11] << 8) | (header_[12] << 16) | (header_[13] << 24);
        width_ = (int32_t)(header_[18] | (header_[19] << 8) | (header_[20] << 16) | (header_[21] << 24));
        height_ = (int32_t)(header_[22] | (header_[23] << 8) | (header_[24] << 16) | (header_[25] << 24));
        uint16_t bpp = header_[28] | (header_[29] << 8);
        uint32_t compression = header_[30] | (header_[31] << 8) | (header_[32] << 16) | (header_[33] << 24);
        absHeight_ = abs(height_);

        if (bpp != 1 || compression != 0 ||
            absHeight_ == 0 || absHeight_ > SCREEN_HEIGHT ||
            width_ <= 0 || width_ > SCREEN_WIDTH) {
          Serial.printf("Unsupported BMP: %ldx%ld %dbpp comp=%lu\n", width_, absHeight_, bpp, compression);
          return sz;
        }

        rowBytes_ = ((width_ + 31) / 32) * 4;
        valid_ = true;
        Serial.printf("BMP: %ldx%ld, rowSize=%d, pixelOffset=%lu\n", width_, absHeight_, rowBytes_, pixelOffset_);
        epd_.fillScreen(TFT_WHITE);
      }
      continue;
    }

    if (!valid_ || done_) {
      continue;
    }

    bytesSeen_++;
    if (bytesSeen_ <= pixelOffset_ - sizeof(header_)) {
      continue;
    }

    row_[rowPos_++] = value;
    if (rowPos_ == rowBytes_) {
      int displayY = (height_ < 0) ? rowsDone_ : (absHeight_ - 1 - rowsDone_);
      for (int x = 0; x < width_; x++) {
        bool isWhite = (row_[x >> 3] >> (7 - (x & 7))) & 0x01;
        epd_.drawPixel(x, displayY, isWhite ? TFT_WHITE : TFT_BLACK);
      }

      rowsDone_++;
      rowPos_ = 0;
      if (rowsDone_ >= absHeight_) {
        done_ = true;
      }
    }
  }

  return sz;
}

size_t BMPDecodeStream::write(uint8_t c) {
  return write(&c, 1);
}

int BMPDecodeStream::available() {
  return 0;
}

int BMPDecodeStream::read() {
  return -1;
}

int BMPDecodeStream::peek() {
  return -1;
}
