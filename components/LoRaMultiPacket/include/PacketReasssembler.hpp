#include <cstdint>
#include <optional>
#include <vector>

#include "Packet.hpp"
/**
 * @class PacketReassembler
 * @brief Manages the reconstruction of split messages from individual Packet chunks.
 *
 * This class handles:
 * - Storage of partial message fragments.
 * - Out-of-order packet insertion.
 * - Reassembly of complete messages.
 * - Timeout-based cleanup of incomplete stale messages.
 */
class PacketReassembler
{
 public:
  std::optional<std::vector<uint8_t>> processPacket(const Packet &packet, uint32_t currentTimestampMs);
  void prune(uint32_t currentTimestampMs, uint32_t timeoutMs);
  void reset();

 private:
}