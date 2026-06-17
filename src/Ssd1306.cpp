#include "Ssd1306.hpp"
#include "font8x8.h"

#include <cstring>
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "Oled";

#define I2C_MASTER_NUM             I2C_NUM_0
#define I2C_MASTER_SDA_IO          17
#define I2C_MASTER_SCL_IO          18
#define I2C_MASTER_FREQ_HZ         400000
#define OLED_I2C_ADDR              0x3C
#define OLED_RST_PIN               GPIO_NUM_21

// SSD1306 Initialization Commands
static const uint8_t OLED_INIT_CMDS[] = {
    0xAE,         // 1. Display Off
    0xD5, 0x80,   // 2. Set Display Clock Divide Ratio/Oscillator Frequency
    0xA8, 0x3F,   // 3. Set Multiplex Ratio (64)
    0xD3, 0x00,   // 4. Set Display Offset
    0x40,         // 5. Set Display Start Line (0)
    0x8D, 0x14,   // 6. Charge Pump Regulator (Enable)
    0x20, 0x00,   // 7. Memory Addressing Mode (Horizontal)
    0xA1,         // 8. Set Segment Re-map (column 127 mapped to SEG0)
    0xC8,         // 9. Set COM Output Scan Direction (remapped)
    0xDA, 0x12,   // 10. Set COM Pins Hardware Configuration
    0x81, 0xCF,   // 11. Set Contrast Control
    0xD9, 0xF1,   // 12. Set Pre-charge Period
    0xDB, 0x40,   // 13. Set VCOMH Deselect Level
    0xA4,         // 14. Entire Display On (Resume to RAM content)
    0xA6,         // 15. Set Normal Display
    0xAF          // 16. Display On
};

Ssd1306::Ssd1306()
{
  std::memset(buffer_, 0, sizeof(buffer_));
}

esp_err_t Ssd1306::init()
{
  // 1. Initialize I2C Master
  i2c_config_t conf = {};
  conf.mode = I2C_MODE_MASTER;
  conf.sda_io_num = I2C_MASTER_SDA_IO;
  conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
  conf.scl_io_num = I2C_MASTER_SCL_IO;
  conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
  conf.master.clk_speed = I2C_MASTER_FREQ_HZ;

  esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &conf);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "I2C Param Config Failed: %d", err);
    return err;
  }

  err = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) // Invalid state means already installed
  {
    ESP_LOGE(TAG, "I2C Driver Install Failed: %d", err);
    return err;
  }

  // 2. Hardware Reset OLED
  gpio_reset_pin(OLED_RST_PIN);
  gpio_set_direction(OLED_RST_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(OLED_RST_PIN, 0);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level(OLED_RST_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(10));

  // 3. Send Initialization Commands
  for (size_t i = 0; i < sizeof(OLED_INIT_CMDS); i++)
  {
    writeCmd(OLED_INIT_CMDS[i]);
  }

  clear();
  update();
  ESP_LOGI(TAG, "SSD1306 OLED Initialized Successfully.");
  return ESP_OK;
}

void Ssd1306::clear()
{
  std::memset(buffer_, 0, sizeof(buffer_));
}

void Ssd1306::print(int row, int col, const char *str)
{
  if (row < 0 || row > 7 || col < 0 || col > 15 || str == nullptr)
    return;

  int buffer_idx = row * 128 + col * 8;

  while (*str != '\0' && buffer_idx < 1024)
  {
    uint8_t c = static_cast<uint8_t>(*str++);
    if (c > 127)
      c = ' '; // Replace non-ascii with space

    const uint8_t *glyph = reinterpret_cast<const uint8_t *>(font8x8_basic[c]);

    // Transpose 8x8 bitmap to SSD1306 vertical page layout
    for (int x = 0; x < 8; ++x)
    {
      uint8_t column_byte = 0;
      for (int y = 0; y < 8; ++y)
      {
        if (glyph[y] & (1 << x))
        {
          column_byte |= (1 << y);
        }
      }
      buffer_[buffer_idx + x] = column_byte;
    }
    buffer_idx += 8;
  }
}

void Ssd1306::update()
{
  // Set horizontal address range: 0 to 127
  writeCmd(0x21);
  writeCmd(0x00);
  writeCmd(0x7F);

  // Set page address range: 0 to 7
  writeCmd(0x22);
  writeCmd(0x00);
  writeCmd(0x07);

  // Transmit buffer
  uint8_t temp[1025];
  temp[0] = 0x40; // Data write prefix
  std::memcpy(temp + 1, buffer_, 1024);

  i2c_master_write_to_device(I2C_MASTER_NUM, OLED_I2C_ADDR, temp, 1025, pdMS_TO_TICKS(500));
}

void Ssd1306::writeCmd(uint8_t cmd)
{
  uint8_t write_buf[2] = {0x00, cmd};
  i2c_master_write_to_device(I2C_MASTER_NUM, OLED_I2C_ADDR, write_buf, 2, pdMS_TO_TICKS(100));
}

void Ssd1306::writeData(const uint8_t *data, size_t len)
{
  if (data == nullptr || len == 0)
    return;

  // Small helper, but update() writes the whole screen
  uint8_t write_buf[129];
  write_buf[0] = 0x40;
  std::memcpy(write_buf + 1, data, len > 128 ? 128 : len);
  i2c_master_write_to_device(I2C_MASTER_NUM, OLED_I2C_ADDR, write_buf, (len > 128 ? 128 : len) + 1, pdMS_TO_TICKS(100));
}
