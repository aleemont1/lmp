#pragma once

#include <map>
#include <string>
#include <vector>

#include "Packet.hpp"

struct DeserializeResult
{
  bool success;
  std::vector<uint8_t> payload;
  std::map<uint8_t, Packet> chunks;  // Map by chunk index for reordering
  size_t paddingBitFlips = 0;        // Count corrupted padding bits for link diagnosis
  std::string error;
};

/**
 * @class PacketDeserializer
 * @brief Static utility class for converting between Packet structures and raw data buffers.
 * This class handles the reconstruction of payloads inside Packet sequences.
 */
class PacketDeserializer
{
 public:
  /**
   * @brief Deserializes a sequence of packets into the original payload.
   *
   * Handles out-of-order packet arrival, validates CRC for each packet,
   * detects padding bit flips, and reconstructs the complete message.
   *
   * @param packets Vector of received packets (order-independent)
   * @return DeserializeResult containing payload, diagnostics, and error info
   */
  static DeserializeResult deserialize(const std::vector<Packet> &packets);

 private:
  /**
   * @brief Extracts valid payload bytes from a single packet.
   *
   * @param packet The packet to extract from
   * @return Vector containing valid payload bytes (respects payloadSize)
   */
  static std::vector<uint8_t> extractPayloadChunk(const Packet &packet);

  /**
   * @brief Validates and counts padding byte corruption.
   *
   * @param packet The packet to validate
   * @return Number of corrupted padding bytes (bit flip indicator)
   */
  static size_t validatePadding(const Packet &packet);
};