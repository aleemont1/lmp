#pragma once

#include <RadioLib.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"

// ==========================================
// FIX: Define missing Arduino constants
// ==========================================
#define LOW 0
#define HIGH 1
#define INPUT 0
#define OUTPUT 1
#define RISING 2
#define FALLING 3

// Heltec V3 Pin Definitions
#define HELTEC_LORA_NSS GPIO_NUM_8
#define HELTEC_LORA_SCK GPIO_NUM_9
#define HELTEC_LORA_MOSI GPIO_NUM_10
#define HELTEC_LORA_MISO GPIO_NUM_11
#define HELTEC_LORA_RST GPIO_NUM_12
#define HELTEC_LORA_BUSY GPIO_NUM_13
#define HELTEC_LORA_DIO1 GPIO_NUM_14
#define HELTEC_POWER_CTRL GPIO_NUM_36

class EspHal : public RadioLibHal
{
 public:
  EspHal(int8_t sck, int8_t miso, int8_t mosi);

  void init() override;

  void pinMode(uint32_t pin, uint32_t mode) override;
  void digitalWrite(uint32_t pin, uint32_t value) override;
  uint32_t digitalRead(uint32_t pin) override;

  void spiBegin() override;
  void spiBeginTransaction() override;
  void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override;
  void spiEndTransaction() override;
  void spiEnd() override;

  void delay(unsigned long ms) override;
  void delayMicroseconds(unsigned long us) override;
  unsigned long millis() override;
  unsigned long micros() override;
  long pulseIn(uint32_t pin, uint32_t state, unsigned long timeout) override;

  void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) override;
  void detachInterrupt(uint32_t interruptNum) override;

 private:
  int8_t _sck, _miso, _mosi;
  spi_device_handle_t spi;
};