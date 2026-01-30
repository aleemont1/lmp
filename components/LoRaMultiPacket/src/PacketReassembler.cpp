#include "PacketReasssembler.hpp"

td::optional<std::vector<uint8_t>> PacketReassembler::processPacket(const Packet &packet, uint32_t currentTimestampMs)
{
  return std::optional<std::vector<uint8_t>>();
}

void PacketReassembler::prune(uint32_t currentTimestampMs, uint32_t timeoutMs)
{
}

void PacketReassembler::reset()
{
}

std::vector<uint8_t> PacketReassembler::reconstruct(const ReassemblySession &session)
{
  return std::vector<uint8_t>();
}
