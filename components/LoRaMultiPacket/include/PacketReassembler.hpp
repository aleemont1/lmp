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

  /**
   * @brief Generates a bitmap of successfully received chunks for a given message ID.
   *
   * @param messageId The message ID of the session.
   * @param bitmapOut Output vector to write the bitmap.
   * @return true if session was found, false otherwise.
   */
  bool getReceivedBitmap(uint16_t messageId, std::vector<uint8_t>& bitmapOut) const;

  /**
   * @brief Checks if a message ID was recently completed.
   */
  bool isCompleted(uint16_t messageId) const;

  /**
   * @brief Marks a message ID as completed, adding it to the history.
   */
  void markCompleted(uint16_t messageId);

 private:
  static constexpr size_t MAX_COMPLETED_HISTORY = 16;
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
    /**
     * @brief Storage for chunks.
     * Maps chunk index -> Packet. Prevents huge pre-allocations.
     */
    std::map<uint8_t, Packet> chunks;

    ReassemblySession(uint8_t total, uint32_t time)
        : totalChunks(total),
          firstReceivedTime(time)
    {
    }
  };

  /**
   * @brief Map of Message ID -> Reassembly Session.
   */
  std::map<uint16_t, ReassemblySession> sessions_;

  /**
   * @brief History of recently completed message IDs.
   */
  std::vector<uint16_t> completedMessages_;

  /**
   * @brief Internal helper to reconstruct payload from a complete session.
   */
  std::vector<uint8_t> reconstruct(const ReassemblySession &session);
};