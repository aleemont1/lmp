// PlatformIO Unity unit tests for LoRaMultiPacket core logic.

#include <unity.h>

#include <cstring>  // for memcmp
#include <vector>

#include "Packet.hpp"
#include "PacketDeserializer.hpp"
#include "PacketParser.hpp"
#include "PacketReassembler.hpp"
#include "PacketSerializer.hpp"
#include "PacketValidator.hpp"

void setUp(void)
{
  // optional setup
}

void tearDown(void)
{
  // optional teardown
}

// ============================================================================
// Packet & Serializer Tests
// ============================================================================

/**
 * @brief Verifies that modifying the payload changes the CRC.
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
 * @brief Verifies that SOM and EOM flags are set correctly.
 */
static void test_packet_flags_multipacket(void)
{
  // 600 bytes will split into: 247 + 247 + 106 (3 packets)
  size_t total = 600;
  std::vector<uint8_t> data(total, 0xAB);

  auto packets = PacketSerializer::splitVectorToPackets(data, 100);

  TEST_ASSERT_EQUAL_INT(3, packets.size());

  // Packet 0: SOM only
  TEST_ASSERT_BITS_HIGH(PACKET_FLAG_SOM, packets[0].header.flags);
  TEST_ASSERT_BITS_LOW(PACKET_FLAG_EOM, packets[0].header.flags);

  // Packet 1: Middle packet
  TEST_ASSERT_EQUAL_HEX8(0x00, packets[1].header.flags);

  // Packet 2: EOM only
  TEST_ASSERT_BITS_LOW(PACKET_FLAG_SOM, packets[2].header.flags);
  TEST_ASSERT_BITS_HIGH(PACKET_FLAG_EOM, packets[2].header.flags);
}

static void test_packet_flags_single_packet(void)
{
  std::vector<uint8_t> data(10, 0xAB);
  auto packets = PacketSerializer::splitVectorToPackets(data, 100);

  TEST_ASSERT_EQUAL_INT(1, packets.size());
  // Should be both SOM (0x01) and EOM (0x02) -> 0x03
  TEST_ASSERT_EQUAL_HEX8(0x03, packets[0].header.flags);
}

/**
 * @brief Verifies binary serialization layout.
 */
static void test_binary_serialization_layout(void)
{
  Packet p{};
  p.header.messageId = 0x1234;
  p.header.payloadSize = 1;
  p.payload.data[0] = 0xEE;
  p.calculateCRC();

  uint8_t buffer[MAX_PACKET_SIZE];
  std::memset(buffer, 0, sizeof(buffer));

  PacketSerializer::serialize(p, buffer);

  // Check Header (Message ID, Little-endian: 0x34, 0x12)
  TEST_ASSERT_EQUAL_HEX8(0x34, buffer[0]);
  TEST_ASSERT_EQUAL_HEX8(0x12, buffer[1]);

  // Check Payload (Offset 7)
  TEST_ASSERT_EQUAL_HEX8(0xEE, buffer[7]);

  // Check CRC
  size_t crcOffset = HEADER_SIZE + sizeof(PacketPayload);
  uint16_t serializedCrc = 0;
  std::memcpy(&serializedCrc, buffer + crcOffset, 2);
  TEST_ASSERT_EQUAL_UINT16(p.crc, serializedCrc);
}

// ============================================================================
// Deserializer, Parser, and Validator Tests
// ============================================================================

static void test_parser_valid_single_chunk(void)
{
  Packet pkt;
  pkt.header.messageId = 1;
  pkt.header.totalChunks = 1;
  pkt.header.chunkIndex = 0;
  pkt.header.payloadSize = 50;
  pkt.header.flags = PACKET_FLAG_SOM | PACKET_FLAG_EOM;
  pkt.header.protocolVersion = 1;

  std::memcpy(pkt.payload.data, "Single chunk packet test", 24);
  pkt.calculateCRC();

  uint8_t buffer[256];
  std::memcpy(buffer, &pkt, sizeof(Packet));

  auto result = PacketParser::parse(buffer, sizeof(Packet));
  TEST_ASSERT_TRUE(result.has_value());
  TEST_ASSERT_EQUAL_UINT16(1, result.value().header.messageId);
}

static void test_parser_rejects_buffer_too_small(void)
{
  uint8_t buffer[10] = {0};
  auto result = PacketParser::parse(buffer, 10);
  TEST_ASSERT_FALSE(result.has_value());
}

static void test_parser_rejects_invalid_protocol_version(void)
{
  Packet pkt;
  pkt.header.protocolVersion = 99;  // Invalid
  pkt.header.payloadSize = 10;
  pkt.calculateCRC();

  uint8_t buffer[256];
  std::memcpy(buffer, &pkt, sizeof(Packet));

  auto result = PacketParser::parse(buffer, sizeof(Packet));
  TEST_ASSERT_FALSE(result.has_value());
}

static void test_parser_rejects_crc_mismatch(void)
{
  Packet pkt;
  pkt.header.messageId = 1;
  pkt.header.payloadSize = 32;
  pkt.header.protocolVersion = 1;
  std::memset(pkt.payload.data, 0xAA, 32);
  pkt.calculateCRC();

  uint8_t buffer[256];
  std::memcpy(buffer, &pkt, sizeof(Packet));

  // Corrupt the CRC
  uint16_t *crc_ptr = reinterpret_cast<uint16_t *>(buffer + HEADER_SIZE + sizeof(PacketPayload));
  *crc_ptr ^= 0xFFFF;

  auto result = PacketParser::parse(buffer, sizeof(Packet));
  TEST_ASSERT_FALSE(result.has_value());
}

static void test_deserializer_extracts_valid_bytes(void)
{
  Packet pkt;
  pkt.header.payloadSize = 38;
  const char testData[] = "Payload deserialization test data here";
  std::memcpy(pkt.payload.data, testData, 38);

  auto extractedPayload = PacketDeserializer::deserialize(pkt);
  TEST_ASSERT_EQUAL_size_t(38, extractedPayload.size());
  TEST_ASSERT_EQUAL_MEMORY(testData, extractedPayload.data(), 38);
}

// ============================================================================
// PacketReassembler Tests
// ============================================================================

/**
 * @brief Helper to generate a dummy packet for reassembly tests.
 */
Packet create_chunk(uint16_t msgId, uint8_t index, uint8_t total, const std::string &content)
{
  Packet p;
  p.header.messageId = msgId;
  p.header.chunkIndex = index;
  p.header.totalChunks = total;
  p.header.payloadSize = content.size();
  p.header.protocolVersion = 1;
  std::memcpy(p.payload.data, content.data(), content.size());
  p.calculateCRC();
  return p;
}

/**
 * @brief Verifies that packets arriving in order are reassembled correctly.
 */
static void test_reassembler_ordered_flow(void)
{
  PacketReassembler reassembler;
  uint32_t time = 1000;

  // Create 3 chunks
  Packet p0 = create_chunk(10, 0, 3, "Hello ");
  Packet p1 = create_chunk(10, 1, 3, "World ");
  Packet p2 = create_chunk(10, 2, 3, "!!!");

  // Feed chunk 0
  auto res0 = reassembler.processPacket(p0, time);
  TEST_ASSERT_FALSE(res0.has_value());  // Not done yet

  // Feed chunk 1
  auto res1 = reassembler.processPacket(p1, time);
  TEST_ASSERT_FALSE(res1.has_value());

  // Feed chunk 2 (Final)
  auto res2 = reassembler.processPacket(p2, time);
  TEST_ASSERT_TRUE(res2.has_value());

  // Verify content
  std::string finalStr(res2.value().begin(), res2.value().end());
  TEST_ASSERT_EQUAL_STRING("Hello World !!!", finalStr.c_str());
}

/**
 * @brief Verifies that packets arriving out of order are reassembled correctly.
 */
static void test_reassembler_unordered_flow(void)
{
  PacketReassembler reassembler;
  uint32_t time = 2000;

  // Create 3 chunks
  Packet p0 = create_chunk(20, 0, 3, "Part1");
  Packet p1 = create_chunk(20, 1, 3, "Part2");
  Packet p2 = create_chunk(20, 2, 3, "Part3");

  // Send Index 2 (Last) first
  auto res2 = reassembler.processPacket(p2, time);
  TEST_ASSERT_FALSE(res2.has_value());

  // Send Index 0 (First)
  auto res0 = reassembler.processPacket(p0, time);
  TEST_ASSERT_FALSE(res0.has_value());

  // Send Index 1 (Middle) - Should trigger completion
  auto res1 = reassembler.processPacket(p1, time);
  TEST_ASSERT_TRUE(res1.has_value());

  // Check data integrity
  std::string result(res1.value().begin(), res1.value().end());
  TEST_ASSERT_EQUAL_STRING("Part1Part2Part3", result.c_str());
}

/**
 * @brief Verifies that duplicate packets are ignored and don't break the counter.
 */
static void test_reassembler_duplicates_ignored(void)
{
  PacketReassembler reassembler;
  uint32_t time = 3000;

  Packet p0 = create_chunk(30, 0, 2, "A");
  Packet p1 = create_chunk(30, 1, 2, "B");

  // Send chunk 0 twice
  reassembler.processPacket(p0, time);
  auto resDup = reassembler.processPacket(p0, time);  // Duplicate
  TEST_ASSERT_FALSE(resDup.has_value());

  // Send chunk 1
  auto resFinal = reassembler.processPacket(p1, time);
  TEST_ASSERT_TRUE(resFinal.has_value());
  TEST_ASSERT_EQUAL_size_t(2, resFinal.value().size());
}

/**
 * @brief Verifies that old sessions are pruned after timeout.
 */
static void test_reassembler_pruning(void)
{
  PacketReassembler reassembler;

  // T=1000: Start Message 40
  Packet p0 = create_chunk(40, 0, 2, "OldData");
  reassembler.processPacket(p0, 1000);

  // T=5000: Prune with timeout 2000ms.
  // Elapsed = 5000 - 1000 = 4000 (> 2000). Should be removed.
  reassembler.prune(5000, 2000);

  // T=5001: Arrive chunk 1 of Message 40.
  // Since session was pruned, this is treated as a *new* partial session
  // containing only chunk 1. It will NOT complete.
  Packet p1 = create_chunk(40, 1, 2, "NewData");
  auto res = reassembler.processPacket(p1, 5001);

  TEST_ASSERT_FALSE(res.has_value());
}

int main(void)
{
  UNITY_BEGIN();

  // Existing Tests
  RUN_TEST(test_crc_changes_on_payload_modification);
  RUN_TEST(test_split_and_reassemble);
  RUN_TEST(test_packet_flags_multipacket);
  RUN_TEST(test_packet_flags_single_packet);
  RUN_TEST(test_binary_serialization_layout);
  RUN_TEST(test_parser_valid_single_chunk);
  RUN_TEST(test_parser_rejects_buffer_too_small);
  RUN_TEST(test_parser_rejects_invalid_protocol_version);
  RUN_TEST(test_parser_rejects_crc_mismatch);
  RUN_TEST(test_deserializer_extracts_valid_bytes);

  // New Reassembler Tests
  RUN_TEST(test_reassembler_ordered_flow);
  RUN_TEST(test_reassembler_unordered_flow);
  RUN_TEST(test_reassembler_duplicates_ignored);
  RUN_TEST(test_reassembler_pruning);

  return UNITY_END();
}

#ifdef ESP_PLATFORM
extern "C" void app_main(void)
{
  main();
}
#endif