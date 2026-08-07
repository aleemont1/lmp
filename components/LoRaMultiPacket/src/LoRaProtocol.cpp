#include "LoRaProtocol.hpp"

#include <cstdio>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char *TAG = "LoRaProto";
#else
#define ESP_LOGI(tag, format, ...) printf("INFO  [%s]: " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) printf("WARN  [%s]: " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, format, ...) printf("ERROR [%s]: " format "\n", tag, ##__VA_ARGS__)
static const char *TAG = "LoRaProto";
#endif

LoRaProtocol::LoRaProtocol(SX1262 *radio, RadioLibHal *hal, uint32_t irqPin)
    : radio_(radio),
      hal_(hal),
      irqPin_(irqPin),
      dropPacketCallback_(nullptr),
      verbose_(false),
      nextMessageId_(1),
      phyBuffer_{0}
{
}

bool LoRaProtocol::send(const std::vector<uint8_t> &data, bool reliable)
{
  if (data.empty())
  {
    return false;
  }

  uint16_t msgId = nextMessageId_++;
  if (nextMessageId_ == 0)
  {
    nextMessageId_ = 1;
  }

  std::vector<Packet> packets = PacketSerializer::splitVectorToPackets(data, msgId);
  if (packets.empty())
  {
    return false;
  }

  if (reliable)
  {
    packets.back().header.flags |= FLAG_ACK_REQ;
    packets.back().calculateCRC();
    return sendReliable(packets);
  }

  return sendUnreliable(packets);
}

bool LoRaProtocol::sendUnreliable(const std::vector<Packet> &packets)
{
  ESP_LOGI(TAG, "Sending MsgID %u (%u chunks, %u bytes total) [Unreliable Mode]",
           packets[0].header.messageId, (unsigned)packets.size(),
           (unsigned)(packets.size() * LORA_MAX_PAYLOAD_SIZE));

  stats_.chunksTx += packets.size();

  for (const auto &packet : packets)
  {
    int state = transmitPacket(packet, "PACKET");
    if (state != RADIOLIB_ERR_NONE)
    {
      ESP_LOGE(TAG, "TX Failed (Chunk %u): %d", packet.header.chunkIndex, state);
      stats_.packetsTxFailed++;
      if (yieldCallback_)
      {
	yieldCallback_();
      }
      return false;
    }
  }

  ESP_LOGI(TAG, "TX Complete.");
  if (yieldCallback_)
  {
    yieldCallback_();
  }

  radio_->startReceive();
  stats_.packetsTx++;
  return true;
}

bool LoRaProtocol::sendReliable(const std::vector<Packet> &packets)
{
  uint16_t msgId = packets[0].header.messageId;
  uint8_t totalChunks = static_cast<uint8_t>(packets.size());
  uint32_t ackTimeoutMs = calculateAckTimeoutMs(totalChunks);

  ESP_LOGI(TAG, "Sending MsgID %u (%u chunks) [Reliable Mode, Timeout=%ums]",
           msgId, (unsigned)totalChunks, (unsigned)ackTimeoutMs);

  stats_.chunksTx += packets.size();

  for (const auto &packet : packets)
  {
    int state = transmitPacket(packet, "PACKET");
    if (state != RADIOLIB_ERR_NONE)
    {
      ESP_LOGE(TAG, "TX Failed (Chunk %u): %d", packet.header.chunkIndex, state);
      stats_.packetsTxFailed++;
      if (yieldCallback_)
      {
	yieldCallback_();
      }
      return false;
    }
  }

  ESP_LOGI(TAG, "Initial TX Complete. Waiting for SACK...");
  if (yieldCallback_)
  {
    yieldCallback_();
  }

  radio_->startReceive();

  int retries = 0;

  while (true)
  {
    std::vector<uint8_t> sackBitmap;
    if (waitForSack(msgId, ackTimeoutMs, sackBitmap))
    {
      auto missingIndices = SackHelper::getMissingChunkIndices(sackBitmap, totalChunks);
      if (missingIndices.empty())
      {
	ESP_LOGI(TAG, "Reliable send successful! All chunks ACKed.");
	stats_.packetsTx++;
	radio_->startReceive();
	return true;
      }

      ESP_LOGI(TAG, "SACK received. Chunks missing: %u", (unsigned)missingIndices.size());
      if (!retransmitMissingChunks(packets, missingIndices))
      {
	return false;
      }
      retries = 0;
      continue;
    }

    int maxRetries = LoRaMultiPacketConfig::MAX_RETRIES;
    if (retries > maxRetries)
    {
      ESP_LOGE(TAG, "Reliable send failed: Max retries exceeded.");
      radio_->startReceive();
      return false;
    }

    ESP_LOGW(TAG, "ACK timeout. Retrying last chunk (%u/%u)", (unsigned)retries, (unsigned)maxRetries);
    Packet retryPkt = packets[totalChunks - 1];
    retryPkt.header.flags |= FLAG_ACK_REQ;
    retryPkt.calculateCRC();

    stats_.chunksTx++;
    int state = transmitPacket(retryPkt, "RETRY PACKET");
    if (state != RADIOLIB_ERR_NONE)
    {
      ESP_LOGE(TAG, "Retry transmit failed: %d", state);
      stats_.packetsTxFailed++;
      return false;
    }
    radio_->startReceive();
  }
}

uint32_t LoRaProtocol::calculateAckTimeoutMs(size_t totalChunks) const
{
  size_t maxPacketLen = HEADER_SIZE + LORA_MAX_PAYLOAD_SIZE + CRC_SIZE;
  uint32_t toaDataMs = radio_->getTimeOnAir(maxPacketLen) / 1000;
  size_t sackLen = HEADER_SIZE + ((totalChunks + 7) / 8) + CRC_SIZE;
  uint32_t toaSackMs = radio_->getTimeOnAir(sackLen) / 1000;

  uint32_t timeoutMs = static_cast<uint32_t>(
      LoRaMultiPacketConfig::ACK_TIMEOUT_SAFETY_FACTOR * (toaDataMs * totalChunks + toaSackMs) +
      LoRaMultiPacketConfig::ACK_TIMEOUT_GUARD_MS);
  return (timeoutMs < LoRaMultiPacketConfig::MIN_ACK_TIMEOUT_MS)
             ? LoRaMultiPacketConfig::MIN_ACK_TIMEOUT_MS
             : timeoutMs;
}

bool LoRaProtocol::waitForSack(uint16_t msgId, uint32_t timeoutMs, std::vector<uint8_t> &sackBitmapOut)
{
  uint32_t startTime = hal_->millis();

  while (hal_->millis() - startTime < timeoutMs)
  {
    if (yieldCallback_)
    {
      yieldCallback_();
    }

    auto pktOpt = tryReceivePacket();
    if (pktOpt.has_value())
    {
      const auto &packet = pktOpt.value();
      if ((packet.header.flags & FLAG_ACK) && (packet.header.messageId == msgId))
      {
	if (verbose_)
	{
	  ESP_LOGI(TAG, "<<< DUMPING RX SACK PACKET <<<");
	  packet.printPacket();
	  ESP_LOGI(TAG, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");
	}

	sackBitmapOut.assign(packet.payload.data, packet.payload.data + packet.header.payloadSize);

	if (verbose_)
	{
	  char hexBuf[128] = {0};
	  size_t offset = 0;
	  for (size_t i = 0; i < sackBitmapOut.size() && offset < sizeof(hexBuf) - 10; ++i)
	  {
	    offset += std::snprintf(hexBuf + offset, sizeof(hexBuf) - offset, "0x%02X ", sackBitmapOut[i]);
	  }
	  ESP_LOGI(TAG, "Received SACK for MsgID %u. Bitmap Bytes: %s", msgId, hexBuf);
	}
	return true;
      }
    }

    hal_->delay(LoRaMultiPacketConfig::POLL_SLEEP_DELAY_MS);
  }

  return false;
}

bool LoRaProtocol::retransmitMissingChunks(const std::vector<Packet> &packets,
                                           const std::vector<uint8_t> &missingIndices)
{
  stats_.chunksTx += missingIndices.size();

  for (size_t k = 0; k < missingIndices.size(); ++k)
  {
    uint8_t idx = missingIndices[k];
    Packet pkt = packets[idx];

    if (k == missingIndices.size() - 1)
    {
      pkt.header.flags |= FLAG_ACK_REQ;
    }
    else
    {
      pkt.header.flags &= ~FLAG_ACK_REQ;
    }
    pkt.calculateCRC();

    int state = transmitPacket(pkt, "RETRANSMIT PACKET");
    if (state != RADIOLIB_ERR_NONE)
    {
      ESP_LOGE(TAG, "Retransmit failed (Chunk %u): %d", idx, state);
      stats_.packetsTxFailed++;
      return false;
    }
  }

  radio_->startReceive();
  return true;
}

void LoRaProtocol::update(uint32_t currentTimestampMs)
{
  reassembler_.prune(currentTimestampMs, LoRaMultiPacketConfig::PRUNE_TIMEOUT_MS);

  auto pktOpt = tryReceivePacket();
  if (pktOpt.has_value())
  {
    stats_.chunksRx++;
    handleIncomingPacket(pktOpt.value(), currentTimestampMs);
  }
}

std::optional<Packet> LoRaProtocol::tryReceivePacket()
{
  if (!hal_->digitalRead(irqPin_))
  {
    return std::nullopt;
  }

  uint32_t irqFlags = radio_->getIrqFlags();

  if (irqFlags & RADIOLIB_SX126X_IRQ_RX_DONE)
  {
    size_t len = radio_->getPacketLength();
    if (len > 0)
    {
      int state = radio_->readData(phyBuffer_, len);
      if (state == RADIOLIB_ERR_NONE)
      {
	auto packetOpt = PacketParser::parse(phyBuffer_, len);
	if (packetOpt.has_value())
	{
	  radio_->startReceive();
	  return packetOpt;
	}
	else
	{
	  ESP_LOGW(TAG, "Packet Parse Error (CRC/Header mismatch)");
	  stats_.packetsFailed++;
	}
      }
      else
      {
	ESP_LOGE(TAG, "Radio readData failed: %d", state);
	stats_.packetsFailed++;
      }
    }
    else
    {
      ESP_LOGW(TAG, "Ghost Packet detected (len=0), clearing IRQ.");
      radio_->clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
    }
    radio_->startReceive();
  }
  else if (irqFlags & (RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_HEADER_ERR | RADIOLIB_SX126X_IRQ_TIMEOUT))
  {
    ESP_LOGW(TAG, "RX Error detected (IRQ: 0x%04X). Restarting RX.", (unsigned)irqFlags);
    stats_.packetsFailed++;
    radio_->clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
    radio_->startReceive();
  }

  return std::nullopt;
}

void LoRaProtocol::handleIncomingPacket(const Packet &packet, uint32_t currentTimestampMs)
{
  if (verbose_)
  {
    ESP_LOGI(TAG, "<<< DUMPING RX PACKET <<<");
    packet.printPacket();
    ESP_LOGI(TAG, ">>>>>>>>>>>>>>>>>>>>>>>>>");
  }

  if (dropPacketCallback_ && dropPacketCallback_(packet))
  {
    ESP_LOGW(TAG, "[SIMULATED LOSS] Artificially dropping Packet: MsgID=%u ChunkIndex=%u/%u",
             (unsigned)packet.header.messageId, (unsigned)(packet.header.chunkIndex + 1), (unsigned)packet.header.totalChunks);
    radio_->startReceive();
    return;
  }

  uint16_t msgId = packet.header.messageId;

  if (packet.header.flags & FLAG_ACK)
  {
    ESP_LOGI(TAG, "Received unexpected SACK packet (ignored outside TX loop)");
  }
  else
  {
    bool isAlreadyCompleted = reassembler_.isCompleted(msgId);
    bool justCompleted = false;
    std::optional<std::vector<uint8_t>> payloadOpt;

    if (!isAlreadyCompleted)
    {
      payloadOpt = reassembler_.processPacket(packet, currentTimestampMs);
      if (payloadOpt.has_value())
      {
	reassembler_.markCompleted(msgId);
	stats_.packetsRx++;
	justCompleted = true;
      }
    }

    if (packet.header.flags & FLAG_ACK_REQ)
    {
      bool allReceived = isAlreadyCompleted || justCompleted;
      sendSACK(msgId, packet.header.totalChunks, allReceived);
    }

    if (justCompleted && onReceive_)
    {
      ESP_LOGI(TAG, "Reassembly Complete! (%u bytes)", (unsigned)payloadOpt.value().size());
      onReceive_(payloadOpt.value(), radio_->getRSSI(), radio_->getSNR());
    }
  }
}

void LoRaProtocol::sendSACK(uint16_t messageId, uint8_t totalChunks, bool allReceived)
{
  std::vector<uint8_t> bitmap;
  if (allReceived)
  {
    bitmap = SackHelper::createFullAckBitmap(totalChunks);
  }
  else
  {
    reassembler_.getReceivedBitmap(messageId, bitmap);
  }

  Packet ackPacket = SackHelper::createSackPacket(messageId, totalChunks, bitmap);

  ESP_LOGI(TAG, "Sending SACK for MsgID %u (Len=%u, AllReceived=%d)",
           messageId, (unsigned)bitmap.size(), allReceived ? 1 : 0);

  hal_->delay(LoRaMultiPacketConfig::SACK_PREAMBLE_GUARD_DELAY_MS);

  int state = transmitPacket(ackPacket, "SACK PACKET");
  if (state != RADIOLIB_ERR_NONE)
  {
    ESP_LOGE(TAG, "Failed to send SACK: %d", state);
  }

  radio_->startReceive();
}

void LoRaProtocol::setOnReceiveCallback(OnReceiveCallback callback)
{
  onReceive_ = callback;
}

void LoRaProtocol::setYieldCallback(YieldCallback callback)
{
  yieldCallback_ = callback;
}

void LoRaProtocol::setDropPacketCallback(DropPacketCallback callback)
{
  dropPacketCallback_ = callback;
}

void LoRaProtocol::setVerbose(bool enable)
{
  verbose_ = enable;
}

int LoRaProtocol::transmitPacket(const Packet &packet, const char *logPrefix)
{
  if (yieldCallback_)
  {
    yieldCallback_();
  }

  PacketSerializer::serialize(packet, phyBuffer_);
  size_t len = HEADER_SIZE + packet.header.payloadSize + CRC_SIZE;

  if (verbose_)
  {
    ESP_LOGI(TAG, ">>> DUMPING TX %s >>>", logPrefix);
    packet.printPacket();
    ESP_LOGI(TAG, "<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<");
  }

  int state = radio_->transmit(phyBuffer_, len);
  if (state == RADIOLIB_ERR_NONE)
  {
    hal_->delay(LoRaMultiPacketConfig::POST_TX_GUARD_DELAY_MS);
  }
  return state;
}