#include "Packet.hpp"

#include <cstddef>
#include <cstdint>

// Minimal Packet implementation for host unit tests. This reproduces the
// CRC calculation and a no-op print function used by tests. It purposely
// avoids any esp-idf headers so it can be built on desktop hosts.

void Packet::calculateCRC()
{
  uint16_t crc = 0xFFFF;
  const uint8_t *data = reinterpret_cast<const uint8_t *>(this);
  size_t length = offsetof(Packet, crc);

  for (size_t i = 0; i < length; i++)
  {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++)
    {
      if (crc & 0x0001)
        crc = (crc >> 1) ^ 0xA001;
      else
        crc = crc >> 1;
    }
  }
  this->crc = crc;
}

void Packet::printPacket()
{
  // No-op for host tests.
}
