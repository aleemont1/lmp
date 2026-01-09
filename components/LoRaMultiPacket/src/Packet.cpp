#include "Packet.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "esp_log.h"

void Packet::calculateCRC()
{
  uint16_t crc = 0xFFFF;
  const uint8_t *data = reinterpret_cast<const uint8_t *>(this);
  size_t length = offsetof(Packet, crc);

  for (size_t i = 0; i < length; i++)
  {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++)
    {
      if (crc & 0x0001)
	crc = (crc >> 1) ^ 0xA001;
      else
	crc = crc >> 1;
    }
  }
  this->crc = crc;
}

void Packet::printPacket()
{
  static const char *TAG = "LoRaMultiPacket";
  ESP_LOGI(TAG, "######## HEADER ########");
  ESP_LOGI(TAG, "Message ID: %u", (unsigned)this->header.messageId);

  // Decode flags using the new constants
  bool som = (this->header.flags & PACKET_FLAG_SOM) != 0;
  bool eom = (this->header.flags & PACKET_FLAG_EOM) != 0;
  bool ackReq = (this->header.flags & PACKET_FLAG_ACK_REQ) != 0;

  ESP_LOGI(TAG, "Flags: 0x%02X (SOM=%d, EOM=%d, ACKReq=%d)",
           (unsigned)this->header.flags, som ? 1 : 0, eom ? 1 : 0, ackReq ? 1 : 0);

  ESP_LOGI(TAG, "Total Chunks: %u", (unsigned)this->header.totalChunks);
  ESP_LOGI(TAG, "Chunk Index (0-based): %u (1-based: %u)", (unsigned)this->header.chunkIndex,
           (unsigned)(this->header.chunkIndex + 1));
  ESP_LOGI(TAG, "Payload Size: %u", (unsigned)this->header.payloadSize);
  ESP_LOGI(TAG, "Protocol Version: %u", (unsigned)this->header.protocolVersion);
  ESP_LOGI(TAG, "######## PAYLOAD ########");

  int toPrint = this->header.payloadSize;
  if (toPrint > (int)LORA_MAX_PAYLOAD_SIZE)
    toPrint = LORA_MAX_PAYLOAD_SIZE;

  char tmp[8];
  std::string line;
  for (int i = 0; i < toPrint; i++)
  {
    std::snprintf(tmp, sizeof(tmp), "%02X ", this->payload.data[i]);
    line += tmp;
  }
  if (line.empty())
    line = "<empty>";
  ESP_LOGI(TAG, "%s", line.c_str());
  ESP_LOGI(TAG, "CRC: 0x%04X", (unsigned)this->crc);
}