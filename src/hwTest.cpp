#ifdef RUN_HW_TEST
#include <RadioLib.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "EspHal.hpp"
#include "LoRaProtocol.hpp"
#include "Ssd1306.hpp"

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ReliableHWTest";

// Global Hardware Instances
EspHal hal_inst(HELTEC_LORA_SCK, HELTEC_LORA_MISO, HELTEC_LORA_MOSI);
EspHal *hal = &hal_inst;
Module radioModule_inst(hal, HELTEC_LORA_NSS, HELTEC_LORA_DIO1, HELTEC_LORA_RST, HELTEC_LORA_BUSY);
SX1262 radio(&radioModule_inst);
LoRaProtocol protocol_inst(&radio, hal, HELTEC_LORA_DIO1);
LoRaProtocol *protocol = &protocol_inst;
Ssd1306 oled;

// Shared state for logging
static esp_err_t oledErr = ESP_FAIL;
static std::string lastRxMsg = "<none>";
static bool hasReceivedAnyPacket = false;
static int testCaseCount = 0;

void updateOled(const std::string &status, const std::string &extra = "")
{
  if (oledErr != ESP_OK) return;
  oled.clear();
  oled.print(0, 0, " LMP HW TEST  ");
  oled.print(2, 0, ("Case: " + std::to_string(testCaseCount)).c_str());
  oled.print(4, 0, ("Status: " + status).c_str());
  oled.print(6, 0, extra.c_str());
  oled.update();
}

extern "C" void app_main(void)
{
  ESP_LOGI(TAG, "=====================================================");
  ESP_LOGI(TAG, "=== LMP Reliable Mode ACK/NACK Hardware Test Active ===");
  ESP_LOGI(TAG, "=====================================================");

  // Initialize Vext Power
  gpio_reset_pin(HELTEC_POWER_CTRL);
  gpio_set_direction(HELTEC_POWER_CTRL, GPIO_MODE_OUTPUT);
  gpio_set_level(HELTEC_POWER_CTRL, 0); // Active LOW
  vTaskDelay(pdMS_TO_TICKS(100));

  // Initialize OLED Display
  oledErr = oled.init();
  updateOled("Initializing...");

  // Initialize HAL & RadioLib
  hal->init();
  int state = radio.begin(868.0);
  if (state != RADIOLIB_ERR_NONE)
  {
    ESP_LOGE(TAG, "Radio Init Failed: %d", state);
    updateOled("Radio FAIL!");
    while (1) vTaskDelay(1000);
  }

  // Radio Configurations
  radio.setSpreadingFactor(7);
  radio.setBandwidth(500.0);
  radio.setCodingRate(5);
  radio.setSyncWord(0x12);
  radio.setOutputPower(15); // moderate power for indoor testing
  radio.setPreambleLength(8);

  // Initialize protocol stack
  protocol->setYieldCallback([]() {
    vTaskDelay(1);
  });
  protocol->setVerbose(true);

  // Get Node Name
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char nodeName[32];
  std::snprintf(nodeName, sizeof(nodeName), "NODE %02X:%02X", mac[4], mac[5]);
  ESP_LOGI(TAG, "My Node Name: %s", nodeName);

#ifdef NODE_MODE_RX
  ESP_LOGI(TAG, "Running in RECEIVER Mode.");
  updateOled("RX Mode", "Waiting Msg...");

  // Set drop packet callback to simulate loss
  // Stateful drop callback to simulate loss: drop chunk indices 1 and 2 ONLY ONCE per message ID.
  protocol->setDropPacketCallback([](const Packet &packet) -> bool {
    if (packet.header.flags & PACKET_FLAG_ACK)
    {
      return false; 
    }
    
    struct DroppedKey {
      uint16_t msgId;
      uint8_t chunkIndex;
      bool operator==(const DroppedKey& o) const { return msgId == o.msgId && chunkIndex == o.chunkIndex; }
    };
    static std::vector<DroppedKey> droppedList;

    if (packet.header.totalChunks >= 3 && (packet.header.chunkIndex == 1 || packet.header.chunkIndex == 2))
    {
      DroppedKey key{packet.header.messageId, packet.header.chunkIndex};
      bool alreadyDropped = false;
      for (const auto& k : droppedList)
      {
        if (k == key) { alreadyDropped = true; break; }
      }
      
      if (!alreadyDropped)
      {
        droppedList.push_back(key);
        ESP_LOGW(TAG, "[LOSS SIMULATION] Drop Callback: MsgID=%u ChunkIndex=%u of %u. DROPPING FOR THE FIRST TIME!", 
                 packet.header.messageId, packet.header.chunkIndex, packet.header.totalChunks);
        return true; 
      }
    }
    return false; 
  });

  protocol->setOnReceiveCallback([](const std::vector<uint8_t> &payload, float rssi, float snr) {
    std::string txt(payload.begin(), payload.end());
    lastRxMsg = txt;
    hasReceivedAnyPacket = true;
    testCaseCount++;
    ESP_LOGI(TAG, "SUCCESS: >>> MESSAGE RECONSTRUCTED & DELIVERED! (Size: %u)", (unsigned)payload.size());
    ESP_LOGI(TAG, "Content: %s", txt.substr(0, 80).c_str());
    ESP_LOGI(TAG, "Metrics: RSSI=%.1f dBm, SNR=%.1f dB", rssi, snr);
    updateOled("SUCCESS RX", "Len: " + std::to_string(payload.size()));
  });

  // Start receive
  radio.clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
  radio.startReceive();

  while (true)
  {
    uint32_t currentMs = pdTICKS_TO_MS(xTaskGetTickCount());
    protocol->update(currentMs);
    vTaskDelay(pdMS_TO_TICKS(20));
  }

#else // NODE_MODE_TX
  ESP_LOGI(TAG, "Running in TRANSMITTER Mode.");
  updateOled("TX Mode", "Ready...");
  vTaskDelay(pdMS_TO_TICKS(3000)); // wait for receiver to settle

  while (true)
  {
    testCaseCount++;
    // Generate test data: exactly 800 bytes (splits into 4 chunks: 3 full of 246 + 1 partial of 62)
    std::string testMsg = "START_RELIABLE_TEST_MSG_ID_" + std::to_string(testCaseCount) + "|";
    while (testMsg.size() < 780) {
      testMsg += "ABCDEF ";
    }
    testMsg += "|END_MSG";
    std::vector<uint8_t> txData(testMsg.begin(), testMsg.end());

    ESP_LOGI(TAG, "--------------------------------------------------------");
    ESP_LOGI(TAG, "TEST CASE %d: Sending %u bytes payload RELIABLY...", testCaseCount, (unsigned)txData.size());
    ESP_LOGI(TAG, "Payload content preview: %s", testMsg.substr(0, 100).c_str());
    updateOled("TX CASE " + std::to_string(testCaseCount), "Sending...");

    uint64_t startTime = hal->millis();
    bool sendSuccess = protocol->send(txData, true); // reliable = true
    uint64_t elapsed = hal->millis() - startTime;

    if (sendSuccess)
    {
      ESP_LOGI(TAG, "TEST CASE %d RESULT: SUCCESS in %llu ms!", testCaseCount, elapsed);
      updateOled("CASE " + std::to_string(testCaseCount) + " OK", std::to_string(elapsed) + "ms");
    }
    else
    {
      ESP_LOGE(TAG, "TEST CASE %d RESULT: FAILED! (Timeout or too many retries) in %llu ms", testCaseCount, elapsed);
      updateOled("CASE " + std::to_string(testCaseCount) + " FAIL", std::to_string(elapsed) + "ms");
    }

    ESP_LOGI(TAG, "--------------------------------------------------------");
    
    // Wait 8 seconds before next test case
    vTaskDelay(pdMS_TO_TICKS(8000));
  }

#endif
}
#endif
