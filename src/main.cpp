#include "Packet.hpp"
#include "PacketSerializer.hpp"

#include <vector>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lmp_app";

extern "C" void app_main(void) {
  esp_log_level_set(TAG, ESP_LOG_INFO);
  ESP_LOGI(TAG, "Starting LoRaMultiPacket example (ESP-IDF)");

  // Example data to split
  std::vector<uint8_t> data;
  for (int i = 0; i < 200; ++i) data.push_back(static_cast<uint8_t>(i & 0xFF));

  auto packets = PacketSerializer::splitVectorToPackets(data, 1);
  ESP_LOGI(TAG, "Created %u packets", (unsigned)packets.size());

  for (size_t i = 0; i < packets.size(); ++i) {
    packets[i].printPacket();
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
// Removed Arduino sketch during migration to ESP-IDF.
// The ESP-IDF application is in `main/src/main.cpp`.

