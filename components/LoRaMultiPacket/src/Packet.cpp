#include "Packet.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
// Mock for native environment (printf or nothing)
#include <iostream>
#define ESP_LOGI(tag, format, ...) printf("LOG [%s]: " format "\n", tag, ##__VA_ARGS__)
#endif

static inline uint16_t updateCRC16Byte(uint16_t crc, uint8_t byte)
{
  crc ^= byte;
  for (uint8_t j = 0; j < 8; j++)
  {
    if (crc & 0x0001)
      crc = (crc >> 1) ^ 0xA001;
    else
      crc = crc >> 1;
  }
  return crc;
}

static inline uint16_t updateCRC16Buffer(uint16_t crc, const uint8_t *data, size_t len)
{
  for (size_t i = 0; i < len; ++i)
  {
    crc = updateCRC16Byte(crc, data[i]);
  }
  return crc;
}

void Packet::calculateCRC()
{
  uint16_t crc = 0xFFFF;
  crc = updateCRC16Buffer(crc, reinterpret_cast<const uint8_t *>(&this->header), HEADER_SIZE);

  size_t bytesToProcess = (this->header.payloadSize > LORA_MAX_PAYLOAD_SIZE)
                              ? LORA_MAX_PAYLOAD_SIZE
                              : this->header.payloadSize;
  crc = updateCRC16Buffer(crc, this->payload.data, bytesToProcess);

  this->crc = crc;
}

void Packet::printPacket() const
{
  static const char *TAG = "LoRaMultiPacket";
  ESP_LOGI(TAG, "######## HEADER ########");
  ESP_LOGI(TAG, "Message ID: %u", (unsigned)this->header.messageId);

  // Decode flags using the new constants
  bool som = (this->header.flags & PACKET_FLAG_SOM) != 0;
  bool eom = (this->header.flags & PACKET_FLAG_EOM) != 0;
  bool ackReq = (this->header.flags & PACKET_FLAG_ACK_REQ) != 0;
  bool ack = (this->header.flags & PACKET_FLAG_ACK) != 0;

  ESP_LOGI(TAG, "Flags: 0x%02X (SOM=%d, EOM=%d, ACKReq=%d, ACK=%d)",
           (unsigned)this->header.flags, som ? 1 : 0, eom ? 1 : 0, ackReq ? 1 : 0, ack ? 1 : 0);

  ESP_LOGI(TAG, "Total Chunks:\t%u", (unsigned)this->header.totalChunks);
  ESP_LOGI(TAG, "Chunk Index:\t%u",  (unsigned)(this->header.chunkIndex + 1));
  ESP_LOGI(TAG, "Payload Size:\t%u", (unsigned)this->header.payloadSize);
  ESP_LOGI(TAG, "Protocol Version:\t%u", (unsigned)this->header.protocolVersion);
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