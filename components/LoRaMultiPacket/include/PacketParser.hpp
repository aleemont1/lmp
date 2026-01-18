#pragma once

#include <cstdint>
#include <optional>

#include "Packet.hpp"
#include "PacketValidator.hpp"

/**
 * @class PacketParser
 * @brief Parses raw packet buffers into validated Packet structures.
 *
 * Converts raw byte buffers from the LoRa radio into Packet structures
 * with validation. This is the first step in the reception pipeline.
 *
 * **Workflow:**
 *   1. Check buffer size (minimum: HEADER_SIZE + PAYLOAD_SIZE + CRC_SIZE)
 *   2. Parse buffer into Packet structure (memcpy)
 *   3. Call PacketValidator::validate() to verify integrity
 *   4. Return validated Packet on success, nullopt on failure
 */
class PacketParser
{
 public:
  /**
   * @brief Parses and validates a raw packet buffer.
   *
   * Converts raw bytes from LoRa radio into a Packet structure and
   * validates all integrity checks before returning.
   *
   * **Checks performed:**
   *   - Buffer contains minimum required bytes
   *   - Header fields are within valid ranges
   *   - CRC validation (covers header + valid payload, excludes padding)
   *   - SOM/EOM flag consistency
   *
   * @param buffer Raw packet buffer from LoRa radio
   * @param length Length of the buffer in bytes
   * @return Validated Packet if all checks pass, std::nullopt on failure
   */
  static std::optional<Packet> parse(const uint8_t *buffer, size_t length);

 private:
  static constexpr size_t MIN_PACKET_SIZE =
      HEADER_SIZE + sizeof(PacketPayload) + CRC_SIZE;
};
