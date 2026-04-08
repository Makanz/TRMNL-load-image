#ifndef BMP_DECODE_H
#define BMP_DECODE_H

#include <Arduino.h>

class EPaper;

class BMPDecodeStream : public Stream {
 public:
  explicit BMPDecodeStream(EPaper& epd);

  bool valid() const;
  int rowsDone() const;

  size_t write(const uint8_t* data, size_t sz) override;
  size_t write(uint8_t c) override;
  int available() override;
  int read() override;
  int peek() override;

 private:
  EPaper& epd_;
  uint8_t header_[62];
  uint8_t headerPos_ = 0;
  uint32_t pixelOffset_ = 0;
  int32_t width_ = 0;
  int32_t height_ = 0;
  int32_t absHeight_ = 0;
  int rowBytes_ = 0;
  uint8_t row_[100];
  int rowPos_ = 0;
  int rowsDone_ = 0;
  uint32_t bytesSeen_ = 0;
  bool valid_ = false;
  bool done_ = false;
};

#endif