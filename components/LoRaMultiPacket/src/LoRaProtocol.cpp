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
    : radio_(radio), hal_(hal), irqPin_(irqPin), dropPacketCallback_(nullptr), verbose_(false), nextMessageId_(1) {}

bool LoRaProtocol::send(const std::vector<uint8_t> &data, bool reliable)
{
  if (data.empty())
    return false;

  // 1. Segmentation: Vector -> Packets with incrementing Message ID
  std::vector<Packet> packets = PacketSerializer::splitVectorToPackets(data, nextMessageId_++);
  if (nextMessageId_ == 0)
  {
    nextMessageId_ = 1; // Skip reserved 0
  }
  if (packets.empty())
    return false;

  // If reliable mode is requested, set ACK_REQ flag on the final packet
  if (reliable)
  {
    packets.back().header.flags |= PACKET_FLAG_ACK_REQ;
    packets.back().calculateCRC();
  }

  ESP_LOGI(TAG, "Sending MsgID %u (%u chunks, %u bytes total)%s",
           packets[0].header.messageId, (unsigned)packets.size(), (unsigned)data.size(),
           reliable ? " [Reliable Mode]" : "");

  // Increment statistics
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
  
  // Restore receive mode after transmission (required in half-duplex)
  radio_->startReceive();

  if (reliable)
  {
    uint16_t msgId = packets[0].header.messageId;
    uint8_t totalChunks = static_cast<uint8_t>(packets.size());
    constexpr uint32_t ACK_TIMEOUT_MS = 1000;
    constexpr int MAX_RETRIES = 5;
    int retries = 0;

    while (true)
    {
      // Wait for SACK
      uint32_t startTime = hal_->millis();
      bool sackReceived = false;
      std::vector<uint8_t> sackBitmap;

      while (hal_->millis() - startTime < ACK_TIMEOUT_MS)
      {
        if (yieldCallback_)
        {
          yieldCallback_();
        }

        if (hal_->digitalRead(irqPin_))
        {
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
                  const auto &packet = packetOpt.value();
                  if ((packet.header.flags & PACKET_FLAG_ACK) && (packet.header.messageId == msgId))
                  {
                    if (verbose_)
                    {
                      ESP_LOGI(TAG, "<<< DUMPING RX SACK PACKET <<<");
                      packet.printPacket();
                      ESP_LOGI(TAG, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>");
                    }
                    sackReceived = true;
                    sackBitmap.assign(packet.payload.data, packet.payload.data + packet.header.payloadSize);
                    
                    char hexBuf[128] = {0};
                    size_t offset = 0;
                    for (size_t i = 0; i < sackBitmap.size() && offset < sizeof(hexBuf) - 10; ++i)
                    {
                      offset += std::snprintf(hexBuf + offset, sizeof(hexBuf) - offset, "0x%02X ", sackBitmap[i]);
                    }
                    ESP_LOGI(TAG, "Received SACK for MsgID %u. Bitmap Bytes: %s", msgId, hexBuf);
                    break;
                  }
                }
              }
            }
            radio_->startReceive();
          }
          else if (irqFlags & (RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_HEADER_ERR))
          {
            radio_->clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
            radio_->startReceive();
          }
        }
        hal_->delay(10);
      }

      if (sackReceived)
      {
        bool allOk = true;
        std::vector<uint8_t> missingIndices;
        for (uint8_t i = 0; i < totalChunks; ++i)
        {
          size_t byteIdx = i / 8;
          size_t bitIdx = i % 8;
          if (byteIdx >= sackBitmap.size() || !(sackBitmap[byteIdx] & (1 << bitIdx)))
          {
            allOk = false;
            missingIndices.push_back(i);
          }
        }

        if (allOk)
        {
          ESP_LOGI(TAG, "Reliable send successful! All chunks ACKed.");
          stats_.packetsTx++;
          radio_->startReceive();
          return true;
        }

        ESP_LOGI(TAG, "SACK received. Chunks missing: %u", (unsigned)missingIndices.size());
        stats_.chunksTx += missingIndices.size();
        for (size_t k = 0; k < missingIndices.size(); ++k)
        {
          uint8_t idx = missingIndices[k];
          Packet pkt = packets[idx];
          if (k == missingIndices.size() - 1)
          {
            pkt.header.flags |= PACKET_FLAG_ACK_REQ;
          }
          else
          {
            pkt.header.flags &= ~PACKET_FLAG_ACK_REQ;
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
        retries = 0; // reset retry counter because we got feedback
        continue;
      }
      else
      {
        retries++;
        if (retries > MAX_RETRIES)
        {
          ESP_LOGE(TAG, "Reliable send failed: Max retries exceeded.");
          radio_->startReceive();
          return false;
        }

        ESP_LOGW(TAG, "ACK timeout. Retrying last chunk (%u/%u)", retries, MAX_RETRIES);
        Packet pkt = packets[totalChunks - 1];
        pkt.header.flags |= PACKET_FLAG_ACK_REQ;
        pkt.calculateCRC();

        stats_.chunksTx++; // Fix metric: increment on timeout retry
        int state = transmitPacket(pkt, "RETRY PACKET");
        if (state != RADIOLIB_ERR_NONE)
        {
          ESP_LOGE(TAG, "Retry transmit failed: %d", state);
          stats_.packetsTxFailed++;
          return false;
        }
        radio_->startReceive();
      }
    }
  }

  stats_.packetsTx++;
  return true;
}

void LoRaProtocol::update(uint32_t currentTimestampMs)
{
  // Increase prune timeout to 15000 ms to support airtime of large multi-chunk messages
  reassembler_.prune(currentTimestampMs, 15000);

  // Poll physical IRQ pin status first, avoiding unnecessary SPI register polling RF noise
  if (hal_->digitalRead(irqPin_))
  {
    uint32_t irqFlags = radio_->getIrqFlags();

    // Case 2: Packet received successfully
    if (irqFlags & RADIOLIB_SX126X_IRQ_RX_DONE)
    {
      // Increment chunks received stats
      stats_.chunksRx++;

      // Retrieve the actual packet length
      size_t len = radio_->getPacketLength();

      if (len > 0)
      {
        int state = radio_->readData(phyBuffer_, len);

        if (state == RADIOLIB_ERR_NONE)
        {
          auto packetOpt = PacketParser::parse(phyBuffer_, len);
          if (packetOpt.has_value())
          {
            const auto &packet = packetOpt.value();

            if (verbose_)
            {
              ESP_LOGI(TAG, "<<< DUMPING RX PACKET <<<");
              packet.printPacket();
              ESP_LOGI(TAG, ">>>>>>>>>>>>>>>>>>>>>>>>>");
            }

            if (dropPacketCallback_ && dropPacketCallback_(packet))
            {
              ESP_LOGW(TAG, "[SIMULATED LOSS] Artificially dropping Packet: MsgID=%u ChunkIndex=%u/%u", 
                       packet.header.messageId, packet.header.chunkIndex + 1, packet.header.totalChunks);
              radio_->startReceive();
              return;
            }
            uint16_t msgId = packet.header.messageId;

            if (packet.header.flags & PACKET_FLAG_ACK)
            {
              // SACK packets are ignored in normal update() since they are handled
              // by the blocking send() wait loop.
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

              if (packet.header.flags & PACKET_FLAG_ACK_REQ)
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
          else
          {
            ESP_LOGW(TAG, "Packet Parsed Error (CRC/Header internal mismatch)");
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
        ESP_LOGW(TAG, "Ghost Packet detected (len=%u), ignoring.", (unsigned)len);
        // Fix: Use clearIrqFlags() which is public
        radio_->clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
      }

      radio_->startReceive();
    }
    // Case 3: Reception errors
    else if (irqFlags & (RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_HEADER_ERR | RADIOLIB_SX126X_IRQ_TIMEOUT))
    {
      ESP_LOGW(TAG, "RX Error detected (IRQ: 0x%04X). Restarting RX.", (unsigned)irqFlags);
      stats_.packetsFailed++;
      // Fix: Use clearIrqFlags()
      radio_->clearIrqFlags(RADIOLIB_SX126X_IRQ_ALL);
      radio_->startReceive();
    }
  }
}

void LoRaProtocol::sendSACK(uint16_t messageId, uint8_t totalChunks, bool allReceived)
{
  Packet ackPacket{};
  ackPacket.header.messageId = messageId;
  ackPacket.header.flags = PACKET_FLAG_ACK;
  ackPacket.header.chunkIndex = 0;
  ackPacket.header.totalChunks = 1;

  std::vector<uint8_t> bitmap;
  if (allReceived)
  {
    size_t bitmapSize = (totalChunks + 7) / 8;
    bitmap.assign(bitmapSize, 0xFF);
  }
  else
  {
    reassembler_.getReceivedBitmap(messageId, bitmap);
  }

  ackPacket.header.payloadSize = static_cast<uint8_t>(bitmap.size());
  std::memcpy(ackPacket.payload.data, bitmap.data(), bitmap.size());
  
  if (bitmap.size() < LORA_MAX_PAYLOAD_SIZE)
  {
    std::memset(ackPacket.payload.data + bitmap.size(), PAYLOAD_PADDING_BYTE,
                LORA_MAX_PAYLOAD_SIZE - bitmap.size());
  }

  ackPacket.calculateCRC();

  char hexBuf[128] = {0};
  size_t offset = 0;
  for (size_t i = 0; i < bitmap.size() && offset < sizeof(hexBuf) - 10; ++i)
  {
    offset += std::snprintf(hexBuf + offset, sizeof(hexBuf) - offset, "0x%02X ", bitmap[i]);
  }
  ESP_LOGI(TAG, "Sending SACK for MsgID %u (Len=%u, AllReceived=%d) Bitmap Bytes: %s", 
           messageId, (unsigned)bitmap.size(), allReceived ? 1 : 0, hexBuf);

  // Give the transmitting node enough time to switch from TX to RX mode.
  // The sender delays 5ms after TX_DONE, then issues SPI commands to enter RX.
  // If we reply instantly, the sender misses the SACK preamble and times out.
  hal_->delay(25);

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
    hal_->delay(5);
  }
  return state;
}