#include "PacketDeserializer.hpp"

std::vector<uint8_t> PacketDeserializer::deserialize(const Packet &packet)
{
  std::vector<uint8_t> payload;

  // Extract only valid payload bytes (up to payloadSize), excluding padding
  const uint8_t *payloadStart = packet.payload.data;
  payload.insert(payload.end(), payloadStart,
                 payloadStart + packet.header.payloadSize);

  return payload;
}
