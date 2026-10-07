#include "PacketReassembler.hpp"

#include <algorithm>

#include "PacketDeserializer.hpp"

std::optional<std::vector<uint8_t>> PacketReassembler::processPacket(const Packet &packet, uint32_t currentTimestampMs)
{
  uint32_t key = sessionKey(packet.header.srcAddr, packet.header.messageId);
  uint8_t chunkIdx = packet.header.chunkIndex;
  uint8_t total = packet.header.totalChunks;

  auto it = sessions_.find(key);
  if (it == sessions_.end())
  {
    if (sessions_.size() >= MAX_CONCURRENT_MESSAGES)
    {
      return std::nullopt;
    }
    it = sessions_.emplace(key, ReassemblySession(total, currentTimestampMs)).first;
  }

  ReassemblySession &session = it->second;
  session.chunks.emplace(chunkIdx, packet);

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

bool PacketReassembler::getReceivedBitmap(uint8_t srcAddr, uint16_t messageId, std::vector<uint8_t> &bitmapOut) const
{
  auto it = sessions_.find(sessionKey(srcAddr, messageId));
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
      bitmapOut[i / 8] |= (1 << (i % 8));
    }
  }
  return true;
}

bool PacketReassembler::isCompleted(uint8_t srcAddr, uint16_t messageId) const
{
  return std::find(completedMessages_.begin(), completedMessages_.end(), sessionKey(srcAddr, messageId)) != completedMessages_.end();
}

void PacketReassembler::markCompleted(uint8_t srcAddr, uint16_t messageId)
{
  if (isCompleted(srcAddr, messageId))
  {
    return;
  }
  if (completedMessages_.size() >= MAX_COMPLETED_HISTORY)
  {
    completedMessages_.erase(completedMessages_.begin());
  }
  completedMessages_.push_back(sessionKey(srcAddr, messageId));
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
