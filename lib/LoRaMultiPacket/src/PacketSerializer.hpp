
#pragma once

#include "Packet.hpp"
#include <vector>

class PacketSerializer {
public:
  // Serialize a single Packet into a contiguous byte buffer. The caller must
  // provide a buffer large enough to hold sizeof(Packet) bytes.
  static void serialize(const Packet &packet, uint8_t *buffer);

  // Split an arbitrary buffer of bytes into a sequence of Packets ready for
  // transmission. The returned vector contains fully populated Packet
  // structures (including crc). packetNumberStart is used as the first
  // Packet.header.messageId and will be incremented for each packet (each
  // logical message produced from this buffer gets a distinct messageId).
  static std::vector<Packet> splitBufferToPackets(const uint8_t *data, size_t length, uint16_t packetNumberStart = 1);

  // Convenience overload that accepts a std::vector<uint8_t>.
  static std::vector<Packet> splitVectorToPackets(const std::vector<uint8_t> &data, uint16_t packetNumberStart = 1);
};
