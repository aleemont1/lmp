// PlatformIO Unity unit tests for LoRaMultiPacket core logic.

#include <unity.h>

#include <cstring>  // for memcmp
#include <vector>

#include "Packet.hpp"
#include "PacketSerializer.hpp"

void setUp(void)
{
  // optional setup
}

void tearDown(void)
{
  // optional teardown
}

/**
 * @brief Verifies that modifying the payload changes the CRC.
 * Root Cause Analysis (Logic): Ensures CRC is actually calculating over the payload data
 * and isn't a static constant or ignoring bytes.
 */
static void test_crc_changes_on_payload_modification(void)
{
  Packet p1{};
  p1.header.payloadSize = 4;
  for (size_t i = 0; i < p1.header.payloadSize; ++i)
  {
    p1.payload.data[i] = static_cast<uint8_t>(i + 1);
  }
  p1.calculateCRC();

  Packet p2 = p1;
  p2.calculateCRC();
  TEST_ASSERT_EQUAL_UINT16(p1.crc, p2.crc);

  // Flip a bit in the first byte
  p2.payload.data[0] ^= 0xFF;
  p2.calculateCRC();
  TEST_ASSERT_NOT_EQUAL(p1.crc, p2.crc);
}

/**
 * @brief Verifies splitting a large vector into multiple packets and reassembling them.
 * Root Cause Analysis (Logic): Ensures the splitting math (chunking) and reassembly loop
 * preserve the exact original data sequence.
 */
static void test_split_and_reassemble(void)
{
  size_t total = 200;
  std::vector<uint8_t> data(total);
  for (size_t i = 0; i < total; ++i)
  {
    data[i] = static_cast<uint8_t>(i & 0xFF);
  }

  // Split into chunks of 99 bytes (200 / 99 = 2 full chunks + 1 partial)
  auto packets = PacketSerializer::splitVectorToPackets(data, 99);

  std::vector<uint8_t> out;
  for (auto &p : packets)
  {
    size_t n = p.header.payloadSize;
    out.insert(out.end(), p.payload.data, p.payload.data + n);

    // Verify CRC integrity for each generated packet
    uint16_t old = p.crc;
    p.calculateCRC();
    TEST_ASSERT_EQUAL_UINT16(old, p.crc);
  }

  TEST_ASSERT_EQUAL_INT((int)data.size(), (int)out.size());
  for (size_t i = 0; i < data.size(); ++i)
  {
    TEST_ASSERT_EQUAL_UINT8(data[i], out[i]);
  }
}

/**
 * @brief Verifies that SOM (Start of Message) and EOM (End of Message) flags are set correctly.
 * * Scenario:
 * - We need 3 packets to verify SOM, None (Middle), and EOM.
 * - LORA_MAX_PAYLOAD_SIZE is ~247 bytes.
 * - We need > 494 bytes to force a 3rd packet.
 */
static void test_packet_flags_multipacket(void)
{
  // 600 bytes will split into: 247 + 247 + 106 (3 packets)
  size_t total = 600;
  std::vector<uint8_t> data(total, 0xAB);

  // The '100' here is just the MessageID, not size.
  auto packets = PacketSerializer::splitVectorToPackets(data, 100);

  TEST_ASSERT_EQUAL_INT(3, packets.size());

  // Packet 0: SOM only (First chunk)
  TEST_ASSERT_BITS_HIGH(PACKET_FLAG_SOM, packets[0].header.flags);
  TEST_ASSERT_BITS_LOW(PACKET_FLAG_EOM, packets[0].header.flags);

  // Packet 1: Middle packet, neither SOM nor EOM
  TEST_ASSERT_EQUAL_HEX8(0x00, packets[1].header.flags);

  // Packet 2: EOM only (Last chunk)
  TEST_ASSERT_BITS_LOW(PACKET_FLAG_SOM, packets[2].header.flags);
  TEST_ASSERT_BITS_HIGH(PACKET_FLAG_EOM, packets[2].header.flags);
}
/**
 * @brief Verifies that a single packet message has BOTH SOM and EOM flags set.
 */
static void test_packet_flags_single_packet(void)
{
  std::vector<uint8_t> data(10, 0xAB);  // Small payload

  auto packets = PacketSerializer::splitVectorToPackets(data, 100);

  TEST_ASSERT_EQUAL_INT(1, packets.size());

  // Should be both SOM (0x01) and EOM (0x02) -> 0x03
  TEST_ASSERT_EQUAL_HEX8(0x03, packets[0].header.flags);
}

/**
 * @brief Verifies that the binary serialization aligns with the struct layout.
 * This is critical because this byte array is what gets sent over the LoRa radio.
 */
static void test_binary_serialization_layout(void)
{
  Packet p{};
  p.header.messageId = 0x1234;
  p.header.payloadSize = 1;
  p.payload.data[0] = 0xEE;
  p.calculateCRC();

  uint8_t buffer[MAX_PACKET_SIZE];
  // Fill buffer with 0x00 to ensure we see what write touches
  std::memset(buffer, 0, sizeof(buffer));

  PacketSerializer::serialize(p, buffer);

  // 1. Check Header (Message ID is at offset 0)
  // Little-endian assumption: 0x34, 0x12
  TEST_ASSERT_EQUAL_HEX8(0x34, buffer[0]);
  TEST_ASSERT_EQUAL_HEX8(0x12, buffer[1]);

  // 2. Check Payload
  // Payload starts after Header. Header size is defined in Packet.hpp
  // We can calculate offset dynamically or hardcode if we know the struct.
  // Based on struct: 2(id)+1(chunks)+1(idx)+1(size)+1(flags)+1(ver) = 7 bytes header.
  // Payload should be at index 7.
  TEST_ASSERT_EQUAL_HEX8(0xEE, buffer[7]);

  // 3. Check CRC
  // CRC is at: Header + PayloadStructSize (not just used payload)
  // PacketPayload struct is fixed size LORA_MAX_PAYLOAD_SIZE.
  size_t crcOffset = HEADER_SIZE + sizeof(PacketPayload);

  // Check that CRC was copied there
  uint16_t serializedCrc = 0;
  std::memcpy(&serializedCrc, buffer + crcOffset, 2);
  TEST_ASSERT_EQUAL_UINT16(p.crc, serializedCrc);
}

int main(void)
{
  UNITY_BEGIN();
  RUN_TEST(test_crc_changes_on_payload_modification);
  RUN_TEST(test_split_and_reassemble);
  RUN_TEST(test_packet_flags_multipacket);
  RUN_TEST(test_packet_flags_single_packet);
  RUN_TEST(test_binary_serialization_layout);
  return UNITY_END();
}

#ifdef ESP_PLATFORM
extern "C" void app_main(void)
{
  main();
}
#endif