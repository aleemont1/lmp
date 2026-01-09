

#include "PacketSerializer.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <vector>

void PacketSerializer::serialize(const Packet &packet, uint8_t *buffer) {
  // Copy header
  std::memcpy(buffer, &packet.header, HEADER_SIZE);
  // Copy payload (full fixed payload size)
  std::memcpy(buffer + HEADER_SIZE, &packet.payload, sizeof(PacketPayload));
  // Copy CRC (2 bytes)
  std::memcpy(buffer + HEADER_SIZE + sizeof(PacketPayload), &packet.crc,
              CRC_SIZE);
}

std::vector<Packet> PacketSerializer::splitBufferToPackets(const uint8_t *data, size_t length, uint16_t packetNumberStart) {
  std::vector<Packet> result;
  if (data == nullptr || length == 0)
    return result;

  size_t offset = 0;
  // Use single messageId for all fragments produced from this buffer. The
  // caller can increment packetNumberStart between independent messages.
  uint16_t messageId = packetNumberStart;
  // Calculate how many chunks will be needed
  uint8_t totalChunks = static_cast<uint8_t>(
      (length + LORA_MAX_PAYLOAD_SIZE - 1) / LORA_MAX_PAYLOAD_SIZE);

  uint8_t chunkIndex = 0; // zero-based
  while (offset < length) {
    Packet packet{};
    packet.header.messageId = messageId;
    packet.header.totalChunks = totalChunks;
    // chunkIndex is zero-based
    packet.header.chunkIndex = chunkIndex;

    size_t remaining = length - offset;
    uint8_t payloadSize =
        static_cast<uint8_t>(std::min(remaining, LORA_MAX_PAYLOAD_SIZE));
    packet.header.payloadSize = payloadSize;

    // Copy payload data
    std::memcpy(packet.payload.data, data + offset, payloadSize);

    // If smaller than max, pad with 0x01 so CRC stays deterministic
    if (payloadSize < LORA_MAX_PAYLOAD_SIZE) {
      std::memset(packet.payload.data + payloadSize, 0x01,
                  LORA_MAX_PAYLOAD_SIZE - payloadSize);
    }

    // Set flags: bit 0 = SOM (start of message), bit 1 = EOM (end of message)
    uint8_t flags = 0;
    if (chunkIndex == 0) flags |= 0x01; // SOM
    if (chunkIndex == (uint8_t)(totalChunks - 1)) flags |= 0x02; // EOM
    packet.header.flags = flags;

    packet.calculateCRC();
    result.push_back(packet);

    offset += payloadSize;
    chunkIndex++;
  }

  return result;
}

std::vector<Packet> PacketSerializer::splitVectorToPackets(const std::vector<uint8_t> &data, uint16_t packetNumberStart) {
  return splitBufferToPackets(data.empty() ? nullptr : data.data(), data.size(), packetNumberStart);
}