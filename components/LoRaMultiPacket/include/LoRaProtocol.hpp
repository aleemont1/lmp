#pragma once

#include <RadioLib.h>
#include <cstdint>
#include <functional>
#include <vector>

#include "Packet.hpp"
#include "PacketParser.hpp"
#include "PacketReassembler.hpp"
#include "PacketSerializer.hpp"

/**
 * @class LoRaProtocol
 * @brief Manages fragmentation, transmission, and reassembly of LoRa packets.
 */
class LoRaProtocol
{
 public:
  using OnReceiveCallback = std::function<void(const std::vector<uint8_t> &data, float rssi, float snr)>;

  /**
   * @brief Constructor accepting the specific SX1262 driver, HAL pointer, and physical IRQ pin.
   */
  explicit LoRaProtocol(SX1262 *radio, RadioLibHal *hal, uint32_t irqPin);

  /**
   * @brief Sends a payload by splitting it into chunks. Blocking.
   */
  bool send(const std::vector<uint8_t> &data, bool reliable = false);

  /**
   * @brief Main loop update. Handles RX polling and reassembly timeouts.
   */
  void update(uint32_t currentTimestampMs);

  void setOnReceiveCallback(OnReceiveCallback callback);

  using YieldCallback = std::function<void()>;
  void setYieldCallback(YieldCallback callback);

  using DropPacketCallback = std::function<bool(const Packet &packet)>;
  void setDropPacketCallback(DropPacketCallback callback);

  void setVerbose(bool enable);

  struct ProtocolStats
  {
    uint32_t chunksTx = 0;
    uint32_t chunksRx = 0;
    uint32_t packetsRx = 0;
    uint32_t packetsFailed = 0;
    uint32_t packetsTx = 0;
    uint32_t packetsTxFailed = 0;
  };

  ProtocolStats getStats() const { return stats_; }
  void resetStats() { stats_ = ProtocolStats(); }

 private:
  SX1262 *radio_;  // Direct pointer to the driver
  RadioLibHal *hal_; // Pointer to hardware HAL
  uint32_t irqPin_;  // Hardware interrupt pin (e.g. DIO1)
  PacketReassembler reassembler_;
  OnReceiveCallback onReceive_;
  YieldCallback yieldCallback_;
  DropPacketCallback dropPacketCallback_;
  ProtocolStats stats_;
  bool verbose_;
  uint16_t nextMessageId_;
  uint8_t phyBuffer_[256];  // Buffer for hardware I/O

  void sendSACK(uint16_t messageId, uint8_t totalChunks, bool allReceived);
  int transmitPacket(const Packet &packet, const char *logPrefix);
};