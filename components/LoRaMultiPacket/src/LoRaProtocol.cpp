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
      onConnRequest_(nullptr),
      dropPacketCallback_(nullptr),
      verbose_(false),
      nextMessageId_(1),
      phyBuffer_{0}
{
  connection_.state = ConnectionState::CLOSED;
}

bool LoRaProtocol::connect(uint32_t timeoutMs)
{
  uint16_t msgId = nextMessageId_++;
  if (nextMessageId_ == 0)
  {
    nextMessageId_ = 1;
  }

  connection_.state = ConnectionState::SYN_SENT;
  connection_.sessionMsgId = msgId;
  stats_.synSent++;

  SynMetadata synReq{};
  synReq.requestedPayloadSize = LORA_MAX_PAYLOAD_SIZE;
  synReq.windowSize = 1;
  synReq.timeoutMs = static_cast<uint16_t>(timeoutMs);

  ESP_LOGI(TAG, "Initiating 3-Way Handshake (MsgID %u, SYN sent)...", msgId);
  sendSyn(msgId, synReq);

  SynAckMetadata synAckResp{};
  if (!waitForSynAck(msgId, timeoutMs, synAckResp))
  {
    ESP_LOGE(TAG, "3-Way Handshake Failed (SYN-ACK Timeout/NACK for MsgID %u)", msgId);
    connection_.state = ConnectionState::CLOSED;
    return false;
  }

  // Send final ACK of the 3-Way Handshake
  Packet ackPacket{};
  ackPacket.header.messageId = msgId;
  ackPacket.header.totalChunks = 1;
  ackPacket.header.chunkIndex = 0;
  ackPacket.header.payloadSize = 0;
  ackPacket.header.flags = FLAG_CONN_ACK;
  ackPacket.header.protocolVersion = LoRaMultiPacketConfig::PROTOCOL_VERSION;
  ackPacket.calculateCRC();

  transmitPacket(ackPacket, "CONN ACK PACKET");
  radio_->startReceive();

  connection_.state = ConnectionState::ESTABLISHED;
  connection_.negotiatedPayloadSize = synAckResp.acceptedPayloadSize;
  connection_.windowSize = synAckResp.windowSize;
  connection_.lastActivityMs = hal_->millis();
  stats_.connEstablished++;

  ESP_LOGI(TAG, "3-Way Handshake SUCCESS! Connection ESTABLISHED with Peer (MsgID %u).", msgId);
  return true;
}

void LoRaProtocol::disconnect()
{
  if (connection_.state != ConnectionState::CLOSED)
  {
    ESP_LOGI(TAG, "Closing connection session (MsgID %u)...", connection_.sessionMsgId);
    connection_.state = ConnectionState::CLOSED;
  }
}

void LoRaProtocol::sendSyn(uint16_t msgId, const SynMetadata &syn)
{
  Packet p{};
  p.header.messageId = msgId;
  p.header.totalChunks = 1;
  p.header.chunkIndex = 0;
  p.header.payloadSize = sizeof(SynMetadata);
  p.header.flags = FLAG_CONN_REQ;
  p.header.protocolVersion = LoRaMultiPacketConfig::PROTOCOL_VERSION;
  std::memcpy(p.payload.data, &syn, sizeof(SynMetadata));
  p.calculateCRC();

  transmitPacket(p, "SYN PACKET");
  radio_->startReceive();
}

void LoRaProtocol::sendSynAck(uint16_t msgId, const SynAckMetadata &synAck)
{
  Packet p{};
  p.header.messageId = msgId;
  p.header.totalChunks = 1;
  p.header.chunkIndex = 0;
  p.header.payloadSize = sizeof(SynAckMetadata);
  p.header.flags = FLAG_CONN_REQ | FLAG_CONN_ACK;
  p.header.protocolVersion = LoRaMultiPacketConfig::PROTOCOL_VERSION;
  std::memcpy(p.payload.data, &synAck, sizeof(SynAckMetadata));
  p.calculateCRC();

  hal_->delay(LoRaMultiPacketConfig::SACK_PREAMBLE_GUARD_DELAY_MS);
  transmitPacket(p, "SYN-ACK PACKET");
  radio_->startReceive();
}

void LoRaProtocol::sendConnNack(uint16_t msgId, ConnNackReason reason)
{
  Packet p{};
  p.header.messageId = msgId;
  p.header.totalChunks = 1;
  p.header.chunkIndex = 0;
  p.header.payloadSize = 1;
  p.header.flags = FLAG_CONN_NACK;
  p.header.protocolVersion = LoRaMultiPacketConfig::PROTOCOL_VERSION;
  p.payload.data[0] = static_cast<uint8_t>(reason);
  p.calculateCRC();

  hal_->delay(LoRaMultiPacketConfig::SACK_PREAMBLE_GUARD_DELAY_MS);
  transmitPacket(p, "CONN-NACK PACKET");
  radio_->startReceive();
}

bool LoRaProtocol::waitForSynAck(uint16_t msgId, uint32_t timeoutMs, SynAckMetadata &synAckOut)
{
  uint32_t startMs = hal_->millis();

  while (hal_->millis() - startMs < timeoutMs)
  {
    auto pktOpt = tryReceivePacket();
    if (pktOpt.has_value())
    {
      const Packet &pkt = pktOpt.value();
      if (pkt.header.messageId == msgId)
      {
	if (pkt.header.flags & FLAG_CONN_NACK)
	{
	  stats_.connNacked++;
	  ESP_LOGW(TAG, "Connection NACK received for MsgID %u (Reason code: %u)",
	           msgId, (unsigned)pkt.payload.data[0]);
	  return false;
	}

	if ((pkt.header.flags & (FLAG_CONN_REQ | FLAG_CONN_ACK)) == (FLAG_CONN_REQ | FLAG_CONN_ACK))
	{
	  if (pkt.header.payloadSize >= sizeof(SynAckMetadata))
	  {
	    std::memcpy(&synAckOut, pkt.payload.data, sizeof(SynAckMetadata));
	    return true;
	  }
	}
      }
    }

    hal_->delay(LoRaMultiPacketConfig::POLL_SLEEP_DELAY_MS);
  }

  return false;
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

  // Inactivity / Idle Timeout Check for active connections
  if (connection_.state == ConnectionState::ESTABLISHED || connection_.state == ConnectionState::SYN_RCVD)
  {
    if (currentTimestampMs - connection_.lastActivityMs > connection_.timeoutMs)
    {
      ESP_LOGW(TAG, "Connection session MsgID %u TIMED OUT due to inactivity (%u ms). Closing connection.",
               connection_.sessionMsgId, (unsigned)(currentTimestampMs - connection_.lastActivityMs));
      connection_.state = ConnectionState::CLOSED;
    }
  }

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

  // Handle 3-Way Handshake Connection Control Frames
  if (packet.header.flags & FLAG_CONN_REQ)
  {
    if (!(packet.header.flags & FLAG_CONN_ACK))  // Pure SYN packet
    {
      stats_.synRcvd++;

      // Failure Mode: Protocol Version Mismatch
      if (packet.header.protocolVersion != LoRaMultiPacketConfig::PROTOCOL_VERSION)
      {
	ESP_LOGE(TAG, "SYN rejected for MsgID %u: Protocol version mismatch (%u != %u)",
	         msgId, (unsigned)packet.header.protocolVersion, (unsigned)LoRaMultiPacketConfig::PROTOCOL_VERSION);
	sendConnNack(msgId, ConnNackReason::UNSUPPORTED_VERSION);
	return;
      }

      SynMetadata synReq{};
      if (packet.header.payloadSize >= sizeof(SynMetadata))
      {
	std::memcpy(&synReq, packet.payload.data, sizeof(SynMetadata));
      }

      // Parameter Negotiation & Clamping
      uint8_t acceptedPayload = synReq.requestedPayloadSize;
      if (acceptedPayload == 0 || acceptedPayload > LORA_MAX_PAYLOAD_SIZE)
      {
	acceptedPayload = LORA_MAX_PAYLOAD_SIZE;  // Clamp to maximum supported payload
      }

      uint8_t acceptedWindow = (synReq.windowSize == 0) ? 1 : synReq.windowSize;

      bool accept = true;
      if (onConnRequest_)
      {
	accept = onConnRequest_(synReq);
      }

      if (accept)
      {
	connection_.state = ConnectionState::SYN_RCVD;
	connection_.sessionMsgId = msgId;
	connection_.negotiatedPayloadSize = acceptedPayload;
	connection_.windowSize = acceptedWindow;
	connection_.lastActivityMs = currentTimestampMs;
	connection_.timeoutMs = (synReq.timeoutMs > 0) ? synReq.timeoutMs : LoRaMultiPacketConfig::DEFAULT_CONN_INACTIVITY_TIMEOUT_MS;

	SynAckMetadata synAckResp{};
	synAckResp.acceptedPayloadSize = acceptedPayload;
	synAckResp.windowSize = acceptedWindow;
	synAckResp.reserved = 0;

	ESP_LOGI(TAG, "SYN received for MsgID %u (ReqPayload=%uB -> AccPayload=%uB). Replying with SYN-ACK...",
	         msgId, (unsigned)synReq.requestedPayloadSize, (unsigned)acceptedPayload);
	sendSynAck(msgId, synAckResp);
      }
      else
      {
	ESP_LOGW(TAG, "SYN received for MsgID %u but application REJECTED connection. Sending NACK.", msgId);
	sendConnNack(msgId, ConnNackReason::REJECTED);
      }
      return;
    }
  }
  else if (packet.header.flags & FLAG_CONN_ACK)  // Final ACK of Handshake
  {
    if (connection_.state == ConnectionState::SYN_RCVD && connection_.sessionMsgId == msgId)
    {
      connection_.state = ConnectionState::ESTABLISHED;
      connection_.lastActivityMs = currentTimestampMs;
      stats_.connEstablished++;
      ESP_LOGI(TAG, "3-Way Handshake SUCCESS! Connection ESTABLISHED (Server Mode, MsgID %u).", msgId);
      radio_->startReceive();
      return;
    }
  }

  if (connection_.state == ConnectionState::ESTABLISHED && connection_.sessionMsgId == msgId)
  {
    connection_.lastActivityMs = currentTimestampMs;
  }

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

void LoRaProtocol::setOnConnectionRequestCallback(OnConnectionRequestCallback callback)
{
  onConnRequest_ = callback;
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