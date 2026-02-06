/**
 * @file halTest.cpp
 * @author Alessandro Monticelli (alessandr.monticell4@studio.unibo.it)
 * @brief Test sketches for SX1262 HAL created for the Heltec LoRa 32v3 modules.
 * @version 0.1
 * @date 2026-02-06
 *
 * @copyright Copyright (c) 2026
 *
 */

/*
# define HAL_TEST_TRANS // Uncomment to test HAL for the Transmitter module.
*/

/*
# define HAL_TEST_RECV // Uncomment to test HAL for the Receiver module.
*/

 #ifdef HAL_TEST_TRANS
#include <RadioLib.h>

#include "EspHal.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HalTest";

// 1. Istanziamo l'HAL con i pin SPI (SCK, MISO, MOSI)
EspHal *hal = new EspHal(HELTEC_LORA_SCK, HELTEC_LORA_MISO, HELTEC_LORA_MOSI);

// 2. Istanziamo il modulo SX1262 usando l'HAL e i pin di controllo (NSS, DIO1, RST, BUSY)
SX1262 radio = new Module(hal, HELTEC_LORA_NSS, HELTEC_LORA_DIO1, HELTEC_LORA_RST, HELTEC_LORA_BUSY);

extern "C" void app_main(void)
{
  ESP_LOGI(TAG, "=== TEST HAL INIZIATO ===");

  // 3. PASSAGGIO CRITICO: Accensione Vext (GPIO 36)
  // Senza questo, il chip LoRa non riceve corrente.
  gpio_reset_pin(HELTEC_POWER_CTRL);
  gpio_set_direction(HELTEC_POWER_CTRL, GPIO_MODE_OUTPUT);
  gpio_set_level(HELTEC_POWER_CTRL, 0);  // LOW = Acceso
  vTaskDelay(pdMS_TO_TICKS(100));        // Aspetta che la tensione si stabilizzi

  // 4. Inizializzazione RadioLib
  // Se l'HAL funziona (SPI ok, GPIO ok), questo metodo restituisce 0 (RADIOLIB_ERR_NONE)
  ESP_LOGI(TAG, "Inizializzazione SX1262...");
  int state = radio.begin(868.0);  // Frequenza 868.0 MHz

  if (state == RADIOLIB_ERR_NONE)
  {
    ESP_LOGI(TAG, "SUCCESSO! Radio inizializzata correttamente.");
  }
  else
  {
    ESP_LOGE(TAG, "FALLITO. Codice errore: %d", state);
    // Se fallisce qui, controlla i pin in EspHal.h o le saldature
    while (true)
      vTaskDelay(1000);
  }

  // 5. Loop di test trasmissione
  while (true)
  {
    ESP_LOGI(TAG, "Tentativo di invio pacchetto RAW...");

    // Invio semplice stringa (senza il tuo protocollo custom)
    state = radio.transmit("Test HAL OK!");

    if (state == RADIOLIB_ERR_NONE)
    {
      ESP_LOGI(TAG, "TX Completata con successo!");
    }
    else
    {
      ESP_LOGE(TAG, "Errore TX: %d", state);
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}
#endif


#ifdef HAL_TEST_RECV
#include <RadioLib.h>

#include <cstring>

#include "EspHal.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HalTestRX";

// 1. Istanziamo l'HAL e il Modulo
EspHal *hal = new EspHal(HELTEC_LORA_SCK, HELTEC_LORA_MISO, HELTEC_LORA_MOSI);
SX1262 radio = new Module(hal, HELTEC_LORA_NSS, HELTEC_LORA_DIO1, HELTEC_LORA_RST, HELTEC_LORA_BUSY);

extern "C" void app_main(void)
{
  ESP_LOGI(TAG, "=== TEST RICEVITORE (BYTE ARRAY) ===");

  // 2. Accensione Vext
  gpio_reset_pin(HELTEC_POWER_CTRL);
  gpio_set_direction(HELTEC_POWER_CTRL, GPIO_MODE_OUTPUT);
  gpio_set_level(HELTEC_POWER_CTRL, 0);
  vTaskDelay(pdMS_TO_TICKS(100));

  // 3. Init Manuale HAL
  hal->init();

  // 4. Avvio Radio
  int state = radio.begin(868.0);
  if (state == RADIOLIB_ERR_NONE)
  {
    ESP_LOGI(TAG, "Radio Inizializzata! In attesa...");
  }
  else
  {
    ESP_LOGE(TAG, "Init Fallito: %d", state);
    while (true)
      vTaskDelay(1000);
  }

  // 5. Loop di Ricezione
  uint8_t rxBuffer[256];  // Buffer statico per i dati grezzi

  while (true)
  {
    // receive() in questo overload accetta (buffer, lunghezza_max)
    state = radio.receive(rxBuffer, sizeof(rxBuffer));

    if (state == RADIOLIB_ERR_NONE)
    {
      // Recuperiamo la lunghezza effettiva del pacchetto ricevuto
      size_t len = radio.getPacketLength();

      ESP_LOGI(TAG, "PACCHETTO RICEVUTO! (Len: %d)", (int)len);

      // Stampiamo il contenuto come stringa (se è testo) o hex
      // Nota: rxBuffer non è necessariamente null-terminated, quindi usiamo il formato %.*s
      ESP_LOGI(TAG, "Dati: %.*s", (int)len, rxBuffer);

      ESP_LOGI(TAG, "RSSI: %.2f dBm", radio.getRSSI());
      ESP_LOGI(TAG, "SNR:  %.2f dB", radio.getSNR());
    }
    else if (state == RADIOLIB_ERR_RX_TIMEOUT)
    {
      // Timeout: normale, riproviamo
    }
    else if (state == RADIOLIB_ERR_CRC_MISMATCH)
    {
      ESP_LOGW(TAG, "Errore CRC");
    }
    else
    {
      ESP_LOGE(TAG, "Errore RX: %d", state);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
#endif