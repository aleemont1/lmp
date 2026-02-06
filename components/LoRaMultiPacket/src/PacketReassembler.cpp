#include "PacketReassembler.hpp"
#include "PacketDeserializer.hpp"

std::optional<std::vector<uint8_t>> PacketReassembler::processPacket(const Packet &packet, uint32_t currentTimestampMs)
{
  uint16_t msgId = packet.header.messageId;
  uint8_t chunkIdx = packet.header.chunkIndex;
  uint8_t total = packet.header.totalChunks;

  // Check if a corresponding session exists.
  auto it = sessions_.find(msgId);

  // If not
  if (it == sessions_.end())
  {
    // Check if we hit the limit for concurrent sessions
    if (sessions_.size() >= MAX_CONCURRENT_MESSAGES)
    {
      // Discard package
      return std::nullopt;
    }

    // Otherwise create a new session for the newly incoming message.
    it = sessions_.emplace(msgId, ReassemblySession(total, currentTimestampMs)).first;
  }

  ReassemblySession &session = it->second;

  // Store the packet (or ignore it if was already saved).
  if (!session.chunks[chunkIdx].has_value())
  {
    session.chunks[chunkIdx] = packet;
    session.chunksReceivedCount++;
  }

  // If all the chunks for the session have been received, return the reconstructed payload.
  if (session.chunksReceivedCount == session.totalChunks)
  {
    std::vector<uint8_t> result = reconstruct(session);
    sessions_.erase(it);
    return result;
  }

  return std::nullopt;
}

void PacketReassembler::prune(uint32_t currentTimestampMs, uint32_t timeoutMs)
{
  auto it = sessions_.begin();
  while (it != sessions_.end())
  {
    if (currentTimestampMs - it->second.firstReceivedTime > timeoutMs)
    {
      it = sessions_.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

void PacketReassembler::reset()
{
  sessions_.clear();
}

std::vector<uint8_t> PacketReassembler::reconstruct(const ReassemblySession &session)
{
  std::vector<uint8_t> fullMessage;

  // Pre-allocate memory assuming a possible full payload (no dummy bytes).
  fullMessage.reserve(session.totalChunks * LORA_MAX_PAYLOAD_SIZE);

  for (const auto &chunkOpt : session.chunks)
  {
    if (chunkOpt.has_value())
    {
      std::vector<uint8_t> chunkData = PacketDeserializer::deserialize(chunkOpt.value());
      fullMessage.insert(fullMessage.end(), chunkData.begin(), chunkData.end());
    }
  }

  return fullMessage;
}
