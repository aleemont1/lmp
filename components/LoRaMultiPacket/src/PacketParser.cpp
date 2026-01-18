#include "PacketParser.hpp"

#include <cstring>

std::optional<Packet> PacketParser::parse(const uint8_t *buffer, size_t length)
{
  // Step 1: Validate buffer size
  if (buffer == nullptr || length < MIN_PACKET_SIZE)
  {
    return std::nullopt;
  }

  // Step 2: Parse buffer into Packet structure
  Packet packet;
  std::memcpy(&packet, buffer, sizeof(Packet));

  // Step 3: Validate packet integrity via PacketValidator
  auto validationError = PacketValidator::validate(packet);
  if (validationError.has_value())
  {
    return std::nullopt;
  }

  // Step 4: Return validated packet
  return packet;
}
