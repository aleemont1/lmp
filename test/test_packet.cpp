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
  // 600 bytes splits into 3 packets: 246 + 246 + 108 bytes
  size_t total = 600;
  std::vector<uint8_t> data(total);
  for (size_t i = 0; i < total; ++i)
  {
    data[i] = static_cast<uint8_t>(i & 0xFF);
  }

  // Split into chunks. The second argument is packetNumberStart (Message ID), not chunk size.
  // Chunk size is fixed to LORA_MAX_PAYLOAD_SIZE (246 bytes).
  auto packets = PacketSerializer::splitVectorToPackets(data, 42);

  TEST_ASSERT_EQUAL_INT(3, packets.size());

  std::vector<uint8_t> out;
  for (size_t i = 0; i < packets.size(); ++i)
  {
    auto &p = packets[i];
    TEST_ASSERT_EQUAL_UINT16(42, p.header.messageId);
    TEST_ASSERT_EQUAL_UINT8(3, p.header.totalChunks);
    TEST_ASSERT_EQUAL_UINT8(i, p.header.chunkIndex);

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
  // 600 bytes splits into: 246 + 246 + 108 (3 packets)
  size_t total = 600;
  std::vector<uint8_t> data(total, 0xAB);

  // The second argument is packetNumberStart (Message ID)
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

  // Check Payload (Offset HEADER_SIZE)
  TEST_ASSERT_EQUAL_HEX8(0xEE, buffer[HEADER_SIZE]);

  // Check CRC
  size_t crcOffset = HEADER_SIZE + p.header.payloadSize;
  uint16_t serializedCrc = 0;
  std::memcpy(&serializedCrc, buffer + crcOffset, 2);
  TEST_ASSERT_EQUAL_UINT16(p.crc, serializedCrc);
}

// ============================================================================
// Deserializer, Parser, and Validator Tests
// ============================================================================

static void test_parser_valid_single_chunk(void)
{
  Packet pkt{};
  pkt.header.messageId = 1;
  pkt.header.totalChunks = 1;
  pkt.header.chunkIndex = 0;
  pkt.header.payloadSize = 24;
  pkt.header.flags = PACKET_FLAG_SOM | PACKET_FLAG_EOM;
  pkt.header.protocolVersion = 1;

  std::memcpy(pkt.payload.data, "Single chunk packet test", 24);
  pkt.calculateCRC();

  uint8_t buffer[256];
  PacketSerializer::serialize(pkt, buffer);
  size_t len = HEADER_SIZE + pkt.header.payloadSize + CRC_SIZE;

  auto result = PacketParser::parse(buffer, len);
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
  Packet pkt{};
  pkt.header.messageId = 1;
  pkt.header.totalChunks = 1;
  pkt.header.chunkIndex = 0;
  pkt.header.flags = PACKET_FLAG_SOM | PACKET_FLAG_EOM;
  pkt.header.protocolVersion = 99;  // Invalid
  pkt.header.payloadSize = 10;
  std::memset(pkt.payload.data, 0, 10);
  pkt.calculateCRC();

  uint8_t buffer[256];
  PacketSerializer::serialize(pkt, buffer);
  size_t len = HEADER_SIZE + pkt.header.payloadSize + CRC_SIZE;

  auto result = PacketParser::parse(buffer, len);
  TEST_ASSERT_FALSE(result.has_value());
}

static void test_parser_rejects_crc_mismatch(void)
{
  Packet pkt{};
  pkt.header.messageId = 1;
  pkt.header.totalChunks = 1;
  pkt.header.chunkIndex = 0;
  pkt.header.flags = PACKET_FLAG_SOM | PACKET_FLAG_EOM;
  pkt.header.payloadSize = 32;
  pkt.header.protocolVersion = 1;
  std::memset(pkt.payload.data, 0xAA, 32);
  pkt.calculateCRC();

  uint8_t buffer[256];
  PacketSerializer::serialize(pkt, buffer);
  size_t len = HEADER_SIZE + pkt.header.payloadSize + CRC_SIZE;

  // Corrupt the CRC at the end of the serialized buffer
  uint16_t *crc_ptr = reinterpret_cast<uint16_t *>(buffer + len - CRC_SIZE);
  *crc_ptr ^= 0xFFFF;

  auto result = PacketParser::parse(buffer, len);
  TEST_ASSERT_FALSE(result.has_value());
}

static void test_deserializer_extracts_valid_bytes(void)
{
  Packet pkt{};
  pkt.header.payloadSize = 38;
  const char testData[] = "Payload deserialization test data here";
  std::memcpy(pkt.payload.data, testData, 38);

  std::vector<uint8_t> extractedPayload;
  PacketDeserializer::deserialize(pkt, extractedPayload);
  TEST_ASSERT_EQUAL_size_t(38, extractedPayload.size());
  TEST_ASSERT_EQUAL_MEMORY(testData, extractedPayload.data(), 38);
}

// Helper for validator tests
static Packet create_valid_base_packet()
{
  Packet p{};
  p.header.messageId = 42;
  p.header.totalChunks = 1;
  p.header.chunkIndex = 0;
  p.header.payloadSize = 10;
  p.header.flags = PACKET_FLAG_SOM | PACKET_FLAG_EOM;
  p.header.protocolVersion = 1;
  std::memset(p.payload.data, 0xAB, 10);
  p.calculateCRC();
  return p;
}

// Validator tests targeting PacketValidator::validate directly
static void test_validator_invalid_protocol_version(void)
{
  Packet p = create_valid_base_packet();
  p.header.protocolVersion = 2; // Supported is 1
  p.calculateCRC();
  auto err = PacketValidator::validate(p);
  TEST_ASSERT_TRUE(err.has_value());
  TEST_ASSERT_EQUAL(ValidationError::Type::INVALID_PROTOCOL_VERSION, err.value().type);
}

static void test_validator_invalid_message_id(void)
{
  Packet p = create_valid_base_packet();
  p.header.messageId = 0; // Reserved
  p.calculateCRC();
  auto err = PacketValidator::validate(p);
  TEST_ASSERT_TRUE(err.has_value());
  TEST_ASSERT_EQUAL(ValidationError::Type::INVALID_MESSAGE_ID, err.value().type);
}

static void test_validator_invalid_total_chunks(void)
{
  Packet p = create_valid_base_packet();
  p.header.totalChunks = 0; // Invalid
  p.calculateCRC();
  auto err = PacketValidator::validate(p);
  TEST_ASSERT_TRUE(err.has_value());
  TEST_ASSERT_EQUAL(ValidationError::Type::INVALID_TOTAL_CHUNKS, err.value().type);
}

static void test_validator_invalid_chunk_index(void)
{
  Packet p = create_valid_base_packet();
  p.header.totalChunks = 3;
  p.header.chunkIndex = 3; // Must be < totalChunks (0, 1, 2)
  p.header.flags = 0; // Clear SOM/EOM to avoid flag errors first
  p.calculateCRC();
  auto err = PacketValidator::validate(p);
  TEST_ASSERT_TRUE(err.has_value());
  TEST_ASSERT_EQUAL(ValidationError::Type::INVALID_CHUNK_INDEX, err.value().type);
}

static void test_validator_invalid_payload_size_too_large(void)
{
  Packet p = create_valid_base_packet();
  p.header.payloadSize = LORA_MAX_PAYLOAD_SIZE + 1; // Exceeds limit
  p.calculateCRC();
  auto err = PacketValidator::validate(p);
  TEST_ASSERT_TRUE(err.has_value());
  TEST_ASSERT_EQUAL(ValidationError::Type::INVALID_PAYLOAD_SIZE, err.value().type);
}

static void test_validator_invalid_payload_size_non_final_partial(void)
{
  Packet p = create_valid_base_packet();
  p.header.totalChunks = 2;
  p.header.chunkIndex = 0;
  p.header.payloadSize = 10; // Non-final chunk must be full size (LORA_MAX_PAYLOAD_SIZE)
  p.header.flags = PACKET_FLAG_SOM; // Valid flags for chunk 0
  p.calculateCRC();
  auto err = PacketValidator::validate(p);
  TEST_ASSERT_TRUE(err.has_value());
  TEST_ASSERT_EQUAL(ValidationError::Type::INVALID_PAYLOAD_SIZE, err.value().type);
}

static void test_validator_invalid_som_flag_missing(void)
{
  Packet p = create_valid_base_packet();
  p.header.totalChunks = 2;
  p.header.chunkIndex = 0;
  p.header.payloadSize = LORA_MAX_PAYLOAD_SIZE;
  p.header.flags = 0; // Missing SOM
  p.calculateCRC();
  auto err = PacketValidator::validate(p);
  TEST_ASSERT_TRUE(err.has_value());
  TEST_ASSERT_EQUAL(ValidationError::Type::INVALID_SOM_FLAG, err.value().type);
}

static void test_validator_invalid_som_flag_unexpected(void)
{
  Packet p = create_valid_base_packet();
  p.header.totalChunks = 2;
  p.header.chunkIndex = 1;
  p.header.payloadSize = 10;
  p.header.flags = PACKET_FLAG_SOM | PACKET_FLAG_EOM; // SOM unexpected on chunk 1
  p.calculateCRC();
  auto err = PacketValidator::validate(p);
  TEST_ASSERT_TRUE(err.has_value());
  TEST_ASSERT_EQUAL(ValidationError::Type::INVALID_SOM_FLAG, err.value().type);
}

static void test_validator_invalid_eom_flag_missing(void)
{
  Packet p = create_valid_base_packet();
  p.header.totalChunks = 2;
  p.header.chunkIndex = 1;
  p.header.payloadSize = 10;
  p.header.flags = 0; // Missing EOM on final chunk
  p.calculateCRC();
  auto err = PacketValidator::validate(p);
  TEST_ASSERT_TRUE(err.has_value());
  TEST_ASSERT_EQUAL(ValidationError::Type::INVALID_EOM_FLAG, err.value().type);
}

static void test_validator_invalid_eom_flag_unexpected(void)
{
  Packet p = create_valid_base_packet();
  p.header.totalChunks = 2;
  p.header.chunkIndex = 0;
  p.header.payloadSize = LORA_MAX_PAYLOAD_SIZE;
  p.header.flags = PACKET_FLAG_SOM | PACKET_FLAG_EOM; // EOM unexpected on chunk 0
  p.calculateCRC();
  auto err = PacketValidator::validate(p);
  TEST_ASSERT_TRUE(err.has_value());
  TEST_ASSERT_EQUAL(ValidationError::Type::INVALID_EOM_FLAG, err.value().type);
}

static void test_validator_crc_mismatch(void)
{
  Packet p = create_valid_base_packet();
  p.crc ^= 0xFFFF; // Corrupt CRC
  auto err = PacketValidator::validate(p);
  TEST_ASSERT_TRUE(err.has_value());
  TEST_ASSERT_EQUAL(ValidationError::Type::CRC_MISMATCH, err.value().type);
}

// ============================================================================
// PacketReassembler Tests
// ============================================================================

/**
 * @brief Helper to generate a dummy packet for reassembly tests.
 */
Packet create_chunk(uint16_t msgId, uint8_t index, uint8_t total, const std::string &content)
{
  Packet p{};
  p.header.messageId = msgId;
  p.header.chunkIndex = index;
  p.header.totalChunks = total;
  p.header.payloadSize = content.size();
  p.header.protocolVersion = 1;
  std::memcpy(p.payload.data, content.data(), content.size());
  
  // Set flags correctly based on chunk index
  uint8_t flags = 0;
  if (index == 0) flags |= PACKET_FLAG_SOM;
  if (index == total - 1) flags |= PACKET_FLAG_EOM;
  p.header.flags = flags;

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

static void test_reassembler_session_limit(void)
{
  PacketReassembler reassembler;
  uint32_t time = 1000;

  // Send 10 packets for 10 different message IDs (all are chunk 0 of 2)
  for (uint16_t msgId = 1; msgId <= 10; ++msgId)
  {
    Packet p = create_chunk(msgId, 0, 2, "A");
    auto res = reassembler.processPacket(p, time);
    TEST_ASSERT_FALSE(res.has_value());
  }

  // Now try to send a packet for an 11th message ID
  Packet p11 = create_chunk(11, 0, 2, "B");
  auto res11 = reassembler.processPacket(p11, time);
  TEST_ASSERT_FALSE(res11.has_value()); // Should be discarded because sessions size >= 10

  // Even if we send the final chunk for message 11, it shouldn't complete
  Packet p11_final = create_chunk(11, 1, 2, "C");
  auto res11_final = reassembler.processPacket(p11_final, time);
  TEST_ASSERT_FALSE(res11_final.has_value());

  // However, if we complete one of the first 10 sessions, e.g., message 5
  Packet p5_final = create_chunk(5, 1, 2, "B");
  auto res5_final = reassembler.processPacket(p5_final, time);
  TEST_ASSERT_TRUE(res5_final.has_value()); // Message 5 completes

  // Now sessions size is 9, so we should be able to start message 12
  Packet p12 = create_chunk(12, 0, 2, "D");
  auto res12 = reassembler.processPacket(p12, time);
  TEST_ASSERT_FALSE(res12.has_value()); // Accepted (not completed yet)

  // And it can complete
  Packet p12_final = create_chunk(12, 1, 2, "E");
  auto res12_final = reassembler.processPacket(p12_final, time);
  TEST_ASSERT_TRUE(res12_final.has_value());
}

static void test_reassembler_duplicate_mismatch_ignored(void)
{
  PacketReassembler reassembler;
  uint32_t time = 1000;

  Packet p0 = create_chunk(50, 0, 2, "A");
  Packet p0_mismatch = create_chunk(50, 0, 2, "X"); // Duplicate with different payload
  Packet p1 = create_chunk(50, 1, 2, "B");

  reassembler.processPacket(p0, time);
  // Send duplicate with different content
  auto resDup = reassembler.processPacket(p0_mismatch, time);
  TEST_ASSERT_FALSE(resDup.has_value());

  // Send final chunk
  auto resFinal = reassembler.processPacket(p1, time);
  TEST_ASSERT_TRUE(resFinal.has_value());

  // Reassembled content should be "AB", not "XB"
  std::string finalStr(resFinal.value().begin(), resFinal.value().end());
  TEST_ASSERT_EQUAL_STRING("AB", finalStr.c_str());
}

static void test_reassembler_reset(void)
{
  PacketReassembler reassembler;
  uint32_t time = 1000;

  Packet p0 = create_chunk(60, 0, 2, "A");
  Packet p1 = create_chunk(60, 1, 2, "B");

  reassembler.processPacket(p0, time);

  // Call reset
  reassembler.reset();

  // Now send chunk 1. Since session was reset, this is treated as a new session
  // containing only chunk 1. It should NOT complete.
  auto res = reassembler.processPacket(p1, time);
  TEST_ASSERT_FALSE(res.has_value());
}

static void test_validator_bypass_ack_flags(void)
{
  Packet ackPacket{};
  ackPacket.header.messageId = 123;
  ackPacket.header.flags = PACKET_FLAG_ACK;
  ackPacket.header.chunkIndex = 0;
  ackPacket.header.totalChunks = 1;
  ackPacket.header.payloadSize = 2;
  ackPacket.payload.data[0] = 0xAA;
  ackPacket.payload.data[1] = 0x55;
  ackPacket.calculateCRC();

  auto err = PacketValidator::validate(ackPacket);
  TEST_ASSERT_FALSE(err.has_value());
}

static void test_reassembler_get_received_bitmap(void)
{
  PacketReassembler reassembler;
  uint32_t time = 1000;

  // 10-chunk message reassembly session
  // Chunks received: 0, 1, 3, 4, 7, 9
  // Chunks missing: 2, 5, 6, 8
  // Expected bitmap (10 bits, 2 bytes):
  // Byte 0: bits 0-7: 1 1 0 1 1 0 0 1 -> 0x9B
  // Byte 1: bits 8-15:
  // bit 0 (chunk 8) = 0 (0)
  // bit 1 (chunk 9) = 1 (2) -> 0x02

  reassembler.processPacket(create_chunk(70, 0, 10, "A"), time);
  reassembler.processPacket(create_chunk(70, 1, 10, "B"), time);
  reassembler.processPacket(create_chunk(70, 3, 10, "D"), time);
  reassembler.processPacket(create_chunk(70, 4, 10, "E"), time);
  reassembler.processPacket(create_chunk(70, 7, 10, "H"), time);
  reassembler.processPacket(create_chunk(70, 9, 10, "J"), time);

  std::vector<uint8_t> bitmap;
  bool found = reassembler.getReceivedBitmap(70, bitmap);
  TEST_ASSERT_TRUE(found);
  TEST_ASSERT_EQUAL_INT(2, bitmap.size());
  TEST_ASSERT_EQUAL_HEX8(0x9B, bitmap[0]);
  TEST_ASSERT_EQUAL_HEX8(0x02, bitmap[1]);
}

static void test_reassembler_completed_messages(void)
{
  PacketReassembler reassembler;
  uint32_t time = 1000;

  TEST_ASSERT_FALSE(reassembler.isCompleted(80));

  // Complete a session
  reassembler.processPacket(create_chunk(80, 0, 2, "A"), time);
  auto res = reassembler.processPacket(create_chunk(80, 1, 2, "B"), time);
  TEST_ASSERT_TRUE(res.has_value());

  // Mark it completed
  reassembler.markCompleted(80);
  TEST_ASSERT_TRUE(reassembler.isCompleted(80));

  // Verify that it is cleared on reset
  reassembler.reset();
  TEST_ASSERT_FALSE(reassembler.isCompleted(80));
}

int main(void)
{
  UNITY_BEGIN();

  // Serializer and structural tests
  RUN_TEST(test_crc_changes_on_payload_modification);
  RUN_TEST(test_split_and_reassemble);
  RUN_TEST(test_packet_flags_multipacket);
  RUN_TEST(test_packet_flags_single_packet);
  RUN_TEST(test_binary_serialization_layout);

  // Parser & Deserializer tests
  RUN_TEST(test_parser_valid_single_chunk);
  RUN_TEST(test_parser_rejects_buffer_too_small);
  RUN_TEST(test_parser_rejects_invalid_protocol_version);
  RUN_TEST(test_parser_rejects_crc_mismatch);
  RUN_TEST(test_deserializer_extracts_valid_bytes);

  // Validator Tests
  RUN_TEST(test_validator_invalid_protocol_version);
  RUN_TEST(test_validator_invalid_message_id);
  RUN_TEST(test_validator_invalid_total_chunks);
  RUN_TEST(test_validator_invalid_chunk_index);
  RUN_TEST(test_validator_invalid_payload_size_too_large);
  RUN_TEST(test_validator_invalid_payload_size_non_final_partial);
  RUN_TEST(test_validator_invalid_som_flag_missing);
  RUN_TEST(test_validator_invalid_som_flag_unexpected);
  RUN_TEST(test_validator_invalid_eom_flag_missing);
  RUN_TEST(test_validator_invalid_eom_flag_unexpected);
  RUN_TEST(test_validator_crc_mismatch);
  RUN_TEST(test_validator_bypass_ack_flags);

  // Reassembler Tests
  RUN_TEST(test_reassembler_ordered_flow);
  RUN_TEST(test_reassembler_unordered_flow);
  RUN_TEST(test_reassembler_duplicates_ignored);
  RUN_TEST(test_reassembler_pruning);
  RUN_TEST(test_reassembler_session_limit);
  RUN_TEST(test_reassembler_duplicate_mismatch_ignored);
  RUN_TEST(test_reassembler_reset);
  RUN_TEST(test_reassembler_get_received_bitmap);
  RUN_TEST(test_reassembler_completed_messages);

  return UNITY_END();
}

#ifdef ESP_PLATFORM
extern "C" void app_main(void)
{
  main();
}
#endif