#pragma once

#include <cstdint>
#include <vector>

#include "Packet.hpp"

/**
 * @class SackHelper
 * @brief Helper utility for building and parsing Selective Acknowledgment (SACK) frames.
 */
class SackHelper
{
 public:
  /**
   * @brief Generates a bitmap with all bits set to 1 for totalChunks.
   */
  static std::vector<uint8_t> createFullAckBitmap(uint8_t totalChunks);

  /**
   * @brief Returns the 0-based indices of chunks that are missing according to the bitmap.
   */
  static std::vector<uint8_t> getMissingChunkIndices(const std::vector<uint8_t> &bitmap, uint8_t totalChunks);

  /**
   * @brief Constructs a validated SACK Packet structure containing the given bitmap.
   */
  static Packet createSackPacket(uint16_t messageId, uint8_t totalChunks, const std::vector<uint8_t> &bitmap);
};
