#include "Packet.hpp"
#include "PacketSerializer.hpp"

#include <iostream>
#include <vector>
#include <cassert>

using std::vector;

static void test_crc_changes_on_payload_modification() {
  Packet p1{};
  // small payload
  p1.header.payloadSize = 4;
  for (size_t i = 0; i < p1.header.payloadSize; ++i) p1.payload.data[i] = static_cast<uint8_t>(i + 1);
  p1.calculateCRC();

  Packet p2 = p1;
  // same packet -> same CRC
  p2.calculateCRC();
  assert(p1.crc == p2.crc);

  // modify one byte -> CRC should differ
  p2.payload.data[0] ^= 0xFF;
  p2.calculateCRC();
  assert(p1.crc != p2.crc);
}

static void test_split_and_reassemble() {
  // create a buffer larger than one chunk to force splitting
  size_t total = 200;
  vector<uint8_t> data(total);
  for (size_t i = 0; i < total; ++i) data[i] = static_cast<uint8_t>(i & 0xFF);

  auto packets = PacketSerializer::splitVectorToPackets(data, 42);
  // reconstruct
  vector<uint8_t> out;
  for (auto &p : packets) {
    size_t n = p.header.payloadSize;
    out.insert(out.end(), p.payload.data, p.payload.data + n);
    // sanity: CRC should match when recalculated
    uint16_t old = p.crc;
    p.calculateCRC();
    assert(old == p.crc);
  }
  assert(out.size() == data.size());
  for (size_t i = 0; i < data.size(); ++i) assert(out[i] == data[i]);
}

int main() {
  try {
    test_crc_changes_on_payload_modification();
    test_split_and_reassemble();
  } catch (const std::exception &e) {
    std::cerr << "Test threw exception: " << e.what() << '\n';
    return 2;
  }
  std::cout << "All unit tests passed\n";
  return 0;
}
