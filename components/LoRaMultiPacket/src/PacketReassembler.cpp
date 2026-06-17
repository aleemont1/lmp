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
  if (session.chunks.find(chunkIdx) == session.chunks.end())
  {
    session.chunks.emplace(chunkIdx, packet);
  }

  // If all the chunks for the session have been received, return the reconstructed payload.
  if (session.chunks.size() == session.totalChunks)
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
  completedMessages_.clear();
}

bool PacketReassembler::getReceivedBitmap(uint16_t messageId, std::vector<uint8_t>& bitmapOut) const
{
  auto it = sessions_.find(messageId);
  if (it == sessions_.end())
  {
    return false;
  }

  const auto &session = it->second;
  size_t bitmapSize = (session.totalChunks + 7) / 8;
  bitmapOut.assign(bitmapSize, 0);

  for (uint8_t i = 0; i < session.totalChunks; ++i)
  {
    if (session.chunks.find(i) != session.chunks.end())
    {
      bitmapOut[i / 8] |= (1 << (i % 8)); // Set bit to 1 if chunk exists
    }
  }
  return true;
}

bool PacketReassembler::isCompleted(uint16_t messageId) const
{
  for (uint16_t id : completedMessages_)
  {
    if (id == messageId)
    {
      return true;
    }
  }
  return false;
}

void PacketReassembler::markCompleted(uint16_t messageId)
{
  if (isCompleted(messageId))
  {
    return;
  }
  if (completedMessages_.size() >= MAX_COMPLETED_HISTORY)
  {
    completedMessages_.erase(completedMessages_.begin());
  }
  completedMessages_.push_back(messageId);
}

std::vector<uint8_t> PacketReassembler::reconstruct(const ReassemblySession &session)
{
  std::vector<uint8_t> fullMessage;

  // Pre-allocate memory assuming a possible full payload (no dummy bytes).
  fullMessage.reserve(session.totalChunks * LORA_MAX_PAYLOAD_SIZE);

  for (uint8_t i = 0; i < session.totalChunks; ++i)
  {
    auto it = session.chunks.find(i);
    if (it != session.chunks.end())
    {
      PacketDeserializer::deserialize(it->second, fullMessage);
    }
  }

  return fullMessage;
}
