#include <cstdint>
#include <map>
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
  /**
   * @brief Processes an incoming packet and attempts to reassemble the full message.
   *
   * If the packet completes a sequence, the full payload is returned.
   * If the sequence is still incomplete, std::nullopt is returned.
   *
   * @param packet The valid packet received from the network.
   * @param currentTimestampMs A distinct timestamp (e.g., millis) to track timeout.
   * @return std::optional<std::vector<uint8_t>> The complete reassembled payload if finished.
   */
  std::optional<std::vector<uint8_t>> processPacket(const Packet &packet, uint32_t currentTimestampMs);

  /**
   * @brief Removes incomplete messages that have exceeded the timeout duration.
   *
   * Should be called periodically to free up memory from lost or incomplete sequences.
   *
   * @param currentTimestampMs The current system time.
   * @param timeoutMs The maximum duration to keep an incomplete message since its first packet arrived.
   */
  void prune(uint32_t currentTimestampMs, uint32_t timeoutMs);

  /**
   * @brief Clears all pending reassembly sessions.
   */
  void reset();

 private:
  /**
   * @brief Maximum number of concurrent messages (sequences) allowed to prevent DoS/Memory exhaustion.
   */
  static constexpr size_t MAX_CONCURRENT_MESSAGES = 10;

  /**
   * @brief Keep track of the received chunks for each msgId, with other metadata.
   *
   */
  struct ReassemblySession
  {
    uint8_t totalChunks;
    uint32_t firstReceivedTime;
    uint32_t chunksReceivedCount;
    /**
     * @brief Storage for chunks.
     * Use std::optional to identify missing gaps (unreceived chunks).
     */
    std::vector<std::optional<Packet>> chunks;

    ReassemblySession(uint8_t total, uint32_t time)
        : totalChunks(total),
          firstReceivedTime(time),
          chunksReceivedCount(0),
          chunks(total, std::nullopt)  // Initialize vector with 'empty' slots
    {
    }
  };

  /**
   * @brief Map of Message ID -> Reassembly Session.
   */
  std::map<uint16_t, ReassemblySession> sessions_;

  /**
   * @brief Internal helper to reconstruct payload from a complete session.
   */
  std::vector<uint8_t> reconstruct(const ReassemblySession &session);
};