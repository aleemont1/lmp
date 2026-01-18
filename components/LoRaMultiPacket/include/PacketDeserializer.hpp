#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "Packet.hpp"
#include "PacketValidator.hpp"

/**
 * @class PacketDeserializer
 * @brief Extracts payload data from validated Packet structures.
 *
 * Converts Packet structures into their payload data, respecting the logical
 * payload size and excluding padding bytes. Performs payload extraction only
 * on packets that have already been validated.
 *
 * **Workflow:**
 *   1. Receive a validated Packet structure
 *   2. Extract only the valid payload bytes (up to header.payloadSize)
 *   3. Return vector containing the actual payload data
 *   4. Padding bytes are automatically excluded
 */
class PacketDeserializer
{
 public:
  /**
   * @brief Extracts payload data from a validated packet.
   *
   * Extracts only the valid payload bytes from a Packet structure,
   * respecting the payloadSize field and excluding padding.
   *
   * **Precondition:** The packet should have been validated via PacketValidator.
   *
   * @param packet The validated packet to extract from
   * @return Vector containing the extracted payload bytes (payloadSize bytes)
   */
  static std::vector<uint8_t> deserialize(const Packet &packet);
};