#include "PacketDeserializer.hpp"

void PacketDeserializer::deserialize(const Packet &packet, std::vector<uint8_t> &targetBuffer)
{
  // Extract only valid payload bytes (up to payloadSize), excluding padding
  const uint8_t *payloadStart = packet.payload.data;
  targetBuffer.insert(targetBuffer.end(), payloadStart,
                 payloadStart + packet.header.payloadSize);
}
