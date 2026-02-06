#include "EspHal.hpp"

#include <cstring>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "EspHal"

EspHal::EspHal(int8_t sck, int8_t miso, int8_t mosi)
    : RadioLibHal(INPUT, OUTPUT, LOW, HIGH, RISING, FALLING),
      _sck(sck),
      _miso(miso),
      _mosi(mosi),
      spi(nullptr) {}  // Inizializza a NULL

void EspHal::init()
{
  spiBegin();
}

void EspHal::pinMode(uint32_t pin, uint32_t mode)
{
  if (pin == HELTEC_LORA_NSS || pin == HELTEC_LORA_RST || pin == HELTEC_LORA_BUSY || pin == HELTEC_LORA_DIO1)
  {
    gpio_reset_pin((gpio_num_t)pin);
  }

  if (mode == OUTPUT)
  {
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
  }
  else
  {
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
  }
}

void EspHal::digitalWrite(uint32_t pin, uint32_t value)
{
  gpio_set_level((gpio_num_t)pin, value);
}

uint32_t EspHal::digitalRead(uint32_t pin)
{
  return gpio_get_level((gpio_num_t)pin);
}

void EspHal::spiBegin()
{
  // Evita di reinizializzare se è già attivo
  if (spi != nullptr)
  {
    ESP_LOGW(TAG, "SPI già inizializzato, salto.");
    return;
  }

  ESP_LOGI(TAG, "Inizializzazione SPI bus...");
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = _mosi;
  buscfg.miso_io_num = _miso;
  buscfg.sclk_io_num = _sck;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = 0;

  // Inizializza il bus FSPI (SPI2)
  esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "Fallito spi_bus_initialize: %s", esp_err_to_name(ret));
    return;
  }

  spi_device_interface_config_t devcfg = {};
  devcfg.clock_speed_hz = 4000000;  // 4 MHz
  devcfg.mode = 0;
  devcfg.spics_io_num = -1;  // Gestito manualmente via digitalWrite (NSS)
  devcfg.queue_size = 7;

  ret = spi_bus_add_device(SPI2_HOST, &devcfg, &spi);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "Fallito spi_bus_add_device: %s", esp_err_to_name(ret));
    spi = nullptr;  // Assicura che sia null
  }
  else
  {
    ESP_LOGI(TAG, "SPI Configurato Correttamente!");
  }
}

void EspHal::spiBeginTransaction() {}

void EspHal::spiTransfer(uint8_t *out, size_t len, uint8_t *in)
{
  // Protezione contro il crash: se SPI non è pronto, non fare nulla
  if (spi == nullptr)
  {
    ESP_LOGE(TAG, "ERRORE CRITICO: Tentativo di spiTransfer con SPI non inizializzato!");
    return;
  }

  spi_transaction_t t;
  memset(&t, 0, sizeof(t));
  t.length = len * 8;
  t.tx_buffer = out;
  t.rx_buffer = in;

  esp_err_t ret = spi_device_transmit(spi, &t);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "Errore trasmissione SPI: %s", esp_err_to_name(ret));
  }
}

void EspHal::spiEndTransaction() {}

void EspHal::spiEnd()
{
  if (spi != nullptr)
  {
    spi_bus_remove_device(spi);
    spi_bus_free(SPI2_HOST);
    spi = nullptr;
  }
}

void EspHal::delay(unsigned long ms)
{
  vTaskDelay(pdMS_TO_TICKS(ms));
}

void EspHal::delayMicroseconds(unsigned long us)
{
  esp_rom_delay_us(us);
}

unsigned long EspHal::millis()
{
  return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

unsigned long EspHal::micros()
{
  return (unsigned long)esp_timer_get_time();
}

long EspHal::pulseIn(uint32_t pin, uint32_t state, unsigned long timeout)
{
  return 0;
}

void EspHal::attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode)
{
  // Polling mode, interrupt non usati
}

void EspHal::detachInterrupt(uint32_t interruptNum)
{
  // Polling mode
}