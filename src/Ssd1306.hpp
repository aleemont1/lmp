#pragma once

#include <cstdint>
#include "esp_err.h"

/**
 * @class Ssd1306
 * @brief Simple, self-contained driver for the SSD1306 OLED display on the Heltec V3 board.
 */
class Ssd1306
{
 public:
  Ssd1306();

  /**
   * @brief Initializes the I2C peripheral, resets the OLED, and sends SSD1306 config commands.
   */
  esp_err_t init();

  /**
   * @brief Clears the internal screen buffer.
   */
  void clear();

  /**
   * @brief Draws a string at the specified text row (0-7) and column (0-15).
   * Uses a standard 8x8 font.
   */
  void print(int row, int col, const char *str);

  /**
   * @brief Flushes the buffer to the OLED screen.
   */
  void update();

 private:
  uint8_t buffer_[1024]; // 128 * 64 pixels / 8 bits = 1024 bytes
  void writeCmd(uint8_t cmd);
  void writeData(const uint8_t *data, size_t len);
};
