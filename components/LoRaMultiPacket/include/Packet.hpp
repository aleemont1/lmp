#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief Maximum raw packet size assumed for transmit buffers (including header and CRC).
 * Many LoRa modules have a FIFO limit (e.g., 256 bytes for SX127x).
 */
constexpr size_t MAX_PACKET_SIZE = 255;

/**
 * @brief Reserved bytes for future use or driver overhead.
 */
constexpr size_t RESERVED_BYTES = 0;

/**
 * @brief Size of the Cyclic Redundancy Check (CRC) suffix in bytes.
 */
constexpr size_t CRC_SIZE = sizeof(uint16_t);

/**
 * @brief The maximum size available for the packet logic after reservations.
 */
constexpr size_t MAX_TX_PACKET_SIZE = MAX_PACKET_SIZE - RESERVED_BYTES;

/**
 * @name Packet Flags
 * @brief Bitmasks for the PacketHeader 'flags' field.
 * @{
 */
constexpr uint8_t PACKET_FLAG_SOM = 0x01;      ///< Start of Message: This packet is the first chunk.
constexpr uint8_t PACKET_FLAG_EOM = 0x02;      ///< End of Message: This packet is the last chunk.
constexpr uint8_t PACKET_FLAG_ACK_REQ = 0x04;  ///< Acknowledgement Requested (optional feature).
/** @} */

#pragma pack(push, 1)  // Ensure no compiler padding is inserted between fields

/**
 * @brief Header structure containing metadata for segmentation and reassembly.
 * Total size: 7 bytes.
 */
struct PacketHeader
{
  /**
   * @brief Unique identifier for a complete message.
   * All chunks belonging to the same large message must share this ID.
   */
  uint16_t messageId = 1;

  /**
   * @brief Total number of chunks this message is split into.
   */
  uint8_t totalChunks = 0;

  /**
   * @brief The sequence index of this specific chunk (0-based).
   * Used to reorder packets if they arrive out of order.
   */
  uint8_t chunkIndex = 0;

  /**
   * @brief Number of valid data bytes in the payload of this packet.
   * Must be <= LORA_MAX_PAYLOAD_SIZE.
   */
  uint8_t payloadSize = 0;

  /**
   * @brief Bitmask of packet attributes (SOM, EOM, ACK).
   * See PACKET_FLAG_* constants.
   */
  uint8_t flags = 0;

  /**
   * @brief Protocol version for compatibility checks.
   */
  uint8_t protocolVersion = 1;
};

/**
 * @brief Size of the packet header in bytes.
 */
constexpr size_t HEADER_SIZE = sizeof(PacketHeader);

/**
 * @brief Maximum bytes available for actual data payload per packet.
 * Calculated as: Total Available - Header - CRC.
 */
constexpr size_t LORA_MAX_PAYLOAD_SIZE = MAX_TX_PACKET_SIZE - HEADER_SIZE - CRC_SIZE;

/**
 * @brief Padding byte value used to fill unused space in the final packet's payload.
 * When the last chunk contains fewer bytes than LORA_MAX_PAYLOAD_SIZE, remaining slots
 * are filled with this value to maintain fixed-size serialization.
 */
constexpr uint8_t PAYLOAD_PADDING_BYTE = 0xFF;

/**
 * @brief Fixed-size container for payload data.
 */
struct PacketPayload
{
  uint8_t data[LORA_MAX_PAYLOAD_SIZE];
};

/**
 * @brief The complete Over-The-Air (OTA) packet structure.
 * This structure maps directly to the byte array sent to the LoRa modem.
 */
struct Packet
{
  PacketHeader header;    ///< Metadata for transport.
  PacketPayload payload;  ///< Application data segment.
  uint16_t crc;           ///< Error detection checksum.

  /**
   * @brief Calculates the CRC-16 of the packet and updates the 'crc' field.
   *
   * **CRC Scope:** Covers the full header + only valid payload bytes (respects payloadSize).
   * Padding bytes in the payload buffer are explicitly excluded from CRC calculation.
   *
   * Rationale: This design decouples integrity checking from physical layout and padding
   * strategy. A bit flip in unused padding does not cause a false CRC failure. The CRC protects
   * the semantic message content, not transport artifacts used to fit hardware constraints.
   *
   * Algorithm: Modbus CRC-16 (polynomial 0xA001, initial value 0xFFFF).
   *
   * Formula: CRC(header || payload[0..payloadSize-1])
   */
  void calculateCRC();

  /**
   * @brief Prints a human-readable summary of the packet to the log output.
   * Useful for debugging transmission logic.
   */
  void printPacket();
};
#pragma pack(pop)