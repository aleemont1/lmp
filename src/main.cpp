#ifndef RUN_PAPER_TEST
#ifndef RUN_HW_TEST
#include <RadioLib.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

#include "EspHal.hpp"
#include "LoRaProtocol.hpp"
#include "Ssd1306.hpp"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "App";

// Global Hardware Instances
EspHal hal_inst(HELTEC_LORA_SCK, HELTEC_LORA_MISO, HELTEC_LORA_MOSI);
EspHal *hal = &hal_inst;
Module radioModule_inst(hal, HELTEC_LORA_NSS, HELTEC_LORA_DIO1, HELTEC_LORA_RST, HELTEC_LORA_BUSY);
SX1262 radio(&radioModule_inst);
LoRaProtocol protocol_inst(&radio, hal, HELTEC_LORA_DIO1);
LoRaProtocol *protocol = &protocol_inst;
Ssd1306 oled;

// Shared State for RX updates
[[maybe_unused]] static std::string lastRxMsg = "<none>";
[[maybe_unused]] static float lastRSSI = 0.0f;
[[maybe_unused]] static float lastSNR = 0.0f;
[[maybe_unused]] static int lastTxStatus = 0;            // 0: None, 1: Success, -1: Failure
[[maybe_unused]] static bool hasReceivedAnyPacket = false;
static esp_err_t oledErr = ESP_FAIL;

void updateOledDisplay(uint32_t currentMs);

extern "C" void app_main(void)
{
  ESP_LOGI(TAG, "=== Starting LoRaMultiPacket Show-case Transceiver ===");

  // 1. Hardware Init: Vext Power ON (controls LoRa module power and OLED display power)
  gpio_reset_pin(HELTEC_POWER_CTRL);
  gpio_set_direction(HELTEC_POWER_CTRL, GPIO_MODE_OUTPUT);
  gpio_set_level(HELTEC_POWER_CTRL, 0); // Active LOW turns power ON
  vTaskDelay(pdMS_TO_TICKS(100));        // Wait for voltage to stabilize

  // 2. Initialize OLED Display
  oledErr = oled.init();
  if (oledErr == ESP_OK)
  {
    oled.print(0, 0, " LMP TELEMETRY ");
    oled.print(2, 0, "Initializing...");
#if defined(NODE_MODE_TX)
    oled.print(4, 0, "Mode: TRANSMIT ");
#elif defined(NODE_MODE_RX)
    oled.print(4, 0, "Mode: RECEIVE  ");
#else
    oled.print(4, 0, "Mode: TRANSCEIVE");
#endif
    oled.update();
  }

  // 3. Initialize HAL & RadioLib Driver
  hal->init();
  ESP_LOGI(TAG, "Starting Radio...");
  int state = radio.begin(868.0); // 868.0 MHz
  if (state != RADIOLIB_ERR_NONE)
  {
    ESP_LOGE(TAG, "Radio Init Failed: %d", state);
    if (oledErr == ESP_OK)
    {
      oled.clear();
      oled.print(2, 0, "Radio Init FAIL!");
      oled.print(4, 0, "Check Hardware");
      oled.update();
    }
    while (1)
      vTaskDelay(1000);
  }

  // 4. Configure Radio Parameters (Transmitter & Receiver matching params)
  radio.setSpreadingFactor(7);
  radio.setBandwidth(500.0);
  radio.setCodingRate(5);
  radio.setSyncWord(0x12);  // Private Network SyncWord
  radio.setOutputPower(22); // High output power for Heltec boards
  radio.setPreambleLength(8);

  // 5. Initialize Protocol Stack (using custom agnostically injected HAL and DIO1 Pin)
  protocol->setYieldCallback([]() {
    updateOledDisplay(pdTICKS_TO_MS(xTaskGetTickCount()));
  });

  // 6. Get Unique Node ID from MAC Address
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char nodeName[32];
  std::snprintf(nodeName, sizeof(nodeName), "NODE %02X:%02X", mac[4], mac[5]);
  ESP_LOGI(TAG, "Device Node ID: %s", nodeName);

#ifndef NODE_MODE_TX
  // 7. Register Protocol RX Callback
  protocol->setOnReceiveCallback([](const std::vector<uint8_t> &payload, float rssi, float snr) {
    std::string txt(payload.begin(), payload.end());
    lastRxMsg = txt;
    lastRSSI = rssi;
    lastSNR = snr;
    hasReceivedAnyPacket = true;
    ESP_LOGI(TAG, ">>> RECONSTRUCTED MESSAGE RECEIVED: %s", txt.c_str());
    ESP_LOGI(TAG, ">>> METRICS: RSSI=%.1f dBm | SNR=%.1f dB | Size=%d bytes", rssi, snr, (int)payload.size());
  });

  // 8. Start Initial Receiver Polling
  radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
  radio.startReceive();
#else
  ESP_LOGI(TAG, "Transmitter Mode: Listening disabled.");
#endif

  // 9. Main Loop Timers
  [[maybe_unused]] uint32_t lastTxTime = 0;
  uint32_t lastOledTime = 0;
  [[maybe_unused]] uint32_t txInterval = 10000; // Send telemetry every 10 seconds

  // Add random jitter to TX timer based on MAC to prevent packet collisions on simultaneous startup
  uint32_t startJitter = (mac[5] % 5) * 1000;
  vTaskDelay(pdMS_TO_TICKS(startJitter));

  ESP_LOGI(TAG, "Transceiver is running and listening...");

  while (true)
  {
    uint32_t currentMs = pdTICKS_TO_MS(xTaskGetTickCount());

    // A. Update protocol state machine (polls DIO1 and processes chunks)
    protocol->update(currentMs);

#ifndef NODE_MODE_RX
    // B. Periodic Telemetry Transmission
    if (currentMs - lastTxTime >= txInterval)
    {
      lastTxTime = currentMs;
      
      // Generate a structured payload of exactly 1000 bytes (will be split into 5 chunks: 4 full + 1 partial)
      std::string msgStr = "START_1000_BYTES_PAYLOAD|";
      while (msgStr.size() < 978) {
        msgStr += "Lorem ipsum dolor sit amet, consectetur adipiscing elit. ";
      }
      if (msgStr.size() > 978) {
        msgStr = msgStr.substr(0, 978);
      }
      msgStr += "|END_OF_1000_BYTES";

      std::vector<uint8_t> txData(msgStr.begin(), msgStr.end());

      ESP_LOGI(TAG, "Transmitting 1000-byte telemetry packet (splits into 5 chunks)...");
      
      if (oledErr == ESP_OK)
      {
        oled.print(7, 0, "STATUS: SENDING ");
        oled.update();
      }

      bool txSuccess = protocol->send(txData);
      
      if (txSuccess) {
        ESP_LOGI(TAG, "1000-byte payload transmitted successfully.");
        lastTxStatus = 1;
      } else {
        ESP_LOGE(TAG, "1000-byte payload transmission failed.");
        lastTxStatus = -1;
      }
    }
#endif

    // C. Periodic OLED display update (every 250ms)
    if (currentMs - lastOledTime >= 250)
    {
      lastOledTime = currentMs;
      updateOledDisplay(currentMs);
    }

    // Yield CPU control slightly to allow other tasks to run
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void updateOledDisplay(uint32_t currentMs)
{
  if (oledErr != ESP_OK)
    return;

  auto stats = protocol->getStats();
  oled.clear();
  
  char headerStr[64];
  char uptimeStr[32];
  std::snprintf(uptimeStr, sizeof(uptimeStr), "Uptime:   %lus", (unsigned long)(currentMs / 1000));

#if defined(NODE_MODE_TX)
  std::snprintf(headerStr, sizeof(headerStr), "* LMP: TX ONLY *");
  oled.print(0, 0, headerStr);
  oled.print(1, 0, uptimeStr);
  oled.print(2, 0, "----------------");

  char txChStr[32];
  std::snprintf(txChStr, sizeof(txChStr), "TX Chunks: %lu", (unsigned long)stats.chunksTx);
  oled.print(3, 0, txChStr);

  char txOkStr[32];
  std::snprintf(txOkStr, sizeof(txOkStr), "TxMsg OK:  %lu", (unsigned long)stats.packetsTx);
  oled.print(4, 0, txOkStr);

  char txErrStr[32];
  std::snprintf(txErrStr, sizeof(txErrStr), "TxMsg Err: %lu", (unsigned long)stats.packetsTxFailed);
  oled.print(5, 0, txErrStr);

  oled.print(6, 0, "----------------");

  char statusStr[32];
  if (lastTxStatus == 0) {
    std::snprintf(statusStr, sizeof(statusStr), "STATUS: IDLE    ");
  } else if (lastTxStatus == 1) {
    std::snprintf(statusStr, sizeof(statusStr), "STATUS: TX OK   ");
  } else {
    std::snprintf(statusStr, sizeof(statusStr), "STATUS: TX FAIL ");
  }
  oled.print(7, 0, statusStr);

#elif defined(NODE_MODE_RX)
  std::snprintf(headerStr, sizeof(headerStr), "* LMP: RX ONLY *");
  oled.print(0, 0, headerStr);
  oled.print(1, 0, uptimeStr);
  oled.print(2, 0, "----------------");

  char rxChStr[32];
  std::snprintf(rxChStr, sizeof(rxChStr), "RX Chunks: %lu", (unsigned long)stats.chunksRx);
  oled.print(3, 0, rxChStr);

  char rxOkStr[32];
  std::snprintf(rxOkStr, sizeof(rxOkStr), "RxMsg OK:  %lu", (unsigned long)stats.packetsRx);
  oled.print(4, 0, rxOkStr);

  char rxErrStr[32];
  std::snprintf(rxErrStr, sizeof(rxErrStr), "RxMsg Err: %lu", (unsigned long)stats.packetsFailed);
  oled.print(5, 0, rxErrStr);

  char signalStr[32];
  if (hasReceivedAnyPacket) {
    std::snprintf(signalStr, sizeof(signalStr), "RSSI:%d SNR:%.1f", (int)lastRSSI, lastSNR);
  } else {
    std::snprintf(signalStr, sizeof(signalStr), "RSSI:--- SNR:---");
  }
  oled.print(6, 0, signalStr);
  oled.print(7, 0, "STATUS: LISTENING");

#else
  std::snprintf(headerStr, sizeof(headerStr), "* LMP: TRANSCEIV*");
  oled.print(0, 0, headerStr);
  oled.print(1, 0, uptimeStr);
  oled.print(2, 0, "----------------");

  char txLine[32];
  std::snprintf(txLine, sizeof(txLine), "TX Ch/Msg: %lu/%lu", (unsigned long)stats.chunksTx, (unsigned long)stats.packetsTx);
  oled.print(3, 0, txLine);

  char rxLine[32];
  std::snprintf(rxLine, sizeof(rxLine), "RX Ch/Msg: %lu/%lu", (unsigned long)stats.chunksRx, (unsigned long)stats.packetsRx);
  oled.print(4, 0, rxLine);

  char errLine[32];
  std::snprintf(errLine, sizeof(errLine), "TX/RX Err: %lu/%lu", (unsigned long)stats.packetsTxFailed, (unsigned long)stats.packetsFailed);
  oled.print(5, 0, errLine);

  char signalStr[32];
  if (hasReceivedAnyPacket) {
    std::snprintf(signalStr, sizeof(signalStr), "RSSI:%d SNR:%.1f", (int)lastRSSI, lastSNR);
  } else {
    std::snprintf(signalStr, sizeof(signalStr), "RSSI:--- SNR:---");
  }
  oled.print(6, 0, signalStr);

  char statusStr[32];
  if (lastTxStatus == 1) {
    std::snprintf(statusStr, sizeof(statusStr), "STATUS: TX OK   ");
  } else if (lastTxStatus == -1) {
    std::snprintf(statusStr, sizeof(statusStr), "STATUS: TX FAIL ");
  } else {
    std::snprintf(statusStr, sizeof(statusStr), "STATUS: LISTENING");
  }
  oled.print(7, 0, statusStr);
#endif
  oled.update();
}
#endif
#endif // RUN_PAPER_TEST
