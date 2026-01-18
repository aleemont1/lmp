#include "PacketValidator.hpp"

std::optional<ValidationError> PacketValidator::validate(const Packet &packet)
{
  // Validate header
  auto headerErr = validateHeader(packet.header);
  if (headerErr.has_value())
    return headerErr;

  // Validate flags
  auto flagErr = validateFlags(packet.header);
  if (flagErr.has_value())
    return flagErr;

  // Validate CRC
  auto crcErr = validateCRC(packet, packet.crc);
  if (crcErr.has_value())
    return crcErr;

  return std::nullopt;
}

std::optional<ValidationError> PacketValidator::validateHeader(
    const PacketHeader &header)
{
  // Check protocol version
  if (header.protocolVersion != SUPPORTED_PROTOCOL_VERSION)
  {
    return ValidationError(
        ValidationError::Type::INVALID_PROTOCOL_VERSION,
        "Protocol version " + std::to_string(header.protocolVersion) +
            " not supported (expected " +
            std::to_string(SUPPORTED_PROTOCOL_VERSION) + ")");
  }

  // Check message ID (0 is reserved)
  if (header.messageId == 0)
  {
    return ValidationError(ValidationError::Type::INVALID_MESSAGE_ID,
                           "Message ID cannot be 0 (reserved value)");
  }

  // Check totalChunks
  if (header.totalChunks == 0)
  {
    return ValidationError(ValidationError::Type::INVALID_TOTAL_CHUNKS,
                           "totalChunks must be >= 1");
  }

  // Check chunkIndex within bounds
  if (header.chunkIndex >= header.totalChunks)
  {
    return ValidationError(
        ValidationError::Type::INVALID_CHUNK_INDEX,
        "chunkIndex (" + std::to_string(header.chunkIndex) +
            ") >= totalChunks (" + std::to_string(header.totalChunks) + ")");
  }

  // Check payloadSize within bounds
  if (header.payloadSize > LORA_MAX_PAYLOAD_SIZE)
  {
    return ValidationError(
        ValidationError::Type::INVALID_PAYLOAD_SIZE,
        "payloadSize (" + std::to_string(header.payloadSize) +
            ") > LORA_MAX_PAYLOAD_SIZE (" +
            std::to_string(LORA_MAX_PAYLOAD_SIZE) + ")");
  }

  // Logical check: if not the last chunk, payload must be full
  bool isLastChunk = (header.chunkIndex == header.totalChunks - 1);
  if (!isLastChunk && header.payloadSize != LORA_MAX_PAYLOAD_SIZE)
  {
    return ValidationError(
        ValidationError::Type::INVALID_PAYLOAD_SIZE,
        "Non-final chunk must have full payload (" +
            std::to_string(LORA_MAX_PAYLOAD_SIZE) + " bytes), got " +
            std::to_string(header.payloadSize));
  }

  return std::nullopt;
}

std::optional<ValidationError> PacketValidator::validateFlags(
    const PacketHeader &header)
{
  bool isFirstChunk = (header.chunkIndex == 0);
  bool isLastChunk = (header.chunkIndex == header.totalChunks - 1);

  bool hasSOM = (header.flags & PACKET_FLAG_SOM) != 0;
  bool hasEOM = (header.flags & PACKET_FLAG_EOM) != 0;

  // First chunk must have SOM flag
  if (isFirstChunk && !hasSOM)
  {
    return ValidationError(ValidationError::Type::INVALID_SOM_FLAG,
                           "Chunk 0 must have SOM (Start of Message) flag set");
  }

  // Non-first chunk must not have SOM flag
  if (!isFirstChunk && hasSOM)
  {
    return ValidationError(
        ValidationError::Type::INVALID_SOM_FLAG,
        "Only chunk 0 can have SOM (Start of Message) flag");
  }

  // Last chunk must have EOM flag
  if (isLastChunk && !hasEOM)
  {
    return ValidationError(ValidationError::Type::INVALID_EOM_FLAG,
                           "Final chunk must have EOM (End of Message) flag set");
  }

  // Non-last chunk must not have EOM flag
  if (!isLastChunk && hasEOM)
  {
    return ValidationError(
        ValidationError::Type::INVALID_EOM_FLAG,
        "Only the final chunk can have EOM (End of Message) flag");
  }

  return std::nullopt;
}

std::optional<ValidationError> PacketValidator::validateCRC(
    const Packet &packet, uint16_t receivedCrc)
{
  // Create a copy to calculate CRC
  Packet tempPacket = packet;
  tempPacket.crc = 0;  // Clear CRC field before calculation
  tempPacket.calculateCRC();

  // Compare calculated CRC with received CRC
  if (tempPacket.crc != receivedCrc)
  {
    return ValidationError(
        ValidationError::Type::CRC_MISMATCH,
        "CRC mismatch: expected 0x" + std::string(4, '0') + ", received 0x" +
            std::string(4, '0'));
  }

  return std::nullopt;
}
