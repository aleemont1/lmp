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
   * @brief Deserializes a packet and appends its valid payload bytes to the target buffer.
   *
   * @param packet The validated packet.
   * @param targetBuffer The buffer to append the extracted payload to.
   */
  static void deserialize(const Packet &packet, std::vector<uint8_t> &targetBuffer);
};