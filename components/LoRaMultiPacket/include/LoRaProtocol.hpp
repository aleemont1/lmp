#pragma once

#include <RadioLib.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "Connection.hpp"
#include "Packet.hpp"
#include "PacketParser.hpp"
#include "PacketReassembler.hpp"
#include "PacketSerializer.hpp"
#include "ProtocolConfig.hpp"
#include "SackHelper.hpp"

/**
 * @class LoRaProtocol
 * @brief Manages fragmentation, transmission, reassembly, and connection lifecycle of LoRa packets.
 */
class LoRaProtocol
{
 public:
  using OnReceiveCallback = std::function<void(const std::vector<uint8_t> &data, float rssi, float snr)>;
  using OnConnectionRequestCallback = std::function<bool(const SynMetadata &syn)>;
  using YieldCallback = std::function<void()>;
  using DropPacketCallback = std::function<bool(const Packet &packet)>;

  /**
   * @brief Constructor accepting the specific SX1262 driver, HAL pointer, and physical IRQ pin.
   */
  explicit LoRaProtocol(SX1262 *radio, RadioLibHal *hal, uint32_t irqPin);

  /**
   * @brief Initiates a 3-way handshake (SYN, SYN-ACK, ACK) to establish a connection session. Blocking.
   * @param timeoutMs Maximum time to wait for handshake completion.
   * @return true if connection established successfully, false otherwise.
   */
  bool connect(uint32_t timeoutMs = LoRaMultiPacketConfig::DEFAULT_SYN_TIMEOUT_MS);

  /**
   * @brief Teardown active connection session.
   */
  void disconnect();

  /**
   * @brief Returns true if a connection session is currently ESTABLISHED.
   */
  bool isConnected() const { return connection_.state == ConnectionState::ESTABLISHED; }

  /**
   * @brief Returns current connection state.
   */
  ConnectionState getConnectionState() const { return connection_.state; }

  /**
   * @brief Sends a payload by splitting it into chunks. Blocking.
   */
  bool send(const std::vector<uint8_t> &data, bool reliable = false);

  /**
   * @brief Main loop update. Handles RX polling, reassembly timeouts, and connection maintenance.
   */
  void update(uint32_t currentTimestampMs);

  void setOnReceiveCallback(OnReceiveCallback callback);
  void setOnConnectionRequestCallback(OnConnectionRequestCallback callback);
  void setYieldCallback(YieldCallback callback);
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
    uint32_t synSent = 0;
    uint32_t synRcvd = 0;
    uint32_t connEstablished = 0;
    uint32_t connNacked = 0;
  };

  const ProtocolStats &getStats() const { return stats_; }
  void resetStats() { stats_ = ProtocolStats(); }

 private:
  SX1262 *radio_;     ///< Pointer to SX1262 driver
  RadioLibHal *hal_;  ///< Pointer to hardware HAL
  uint32_t irqPin_;   ///< Hardware interrupt pin (e.g. DIO1)
  PacketReassembler reassembler_;
  ConnectionSession connection_;
  OnReceiveCallback onReceive_;
  OnConnectionRequestCallback onConnRequest_;
  YieldCallback yieldCallback_;
  DropPacketCallback dropPacketCallback_;
  ProtocolStats stats_;
  bool verbose_;
  uint16_t nextMessageId_;
  uint8_t phyBuffer_[LoRaMultiPacketConfig::PHY_BUFFER_SIZE];  ///< Shared buffer for hardware I/O

  // Internal helper methods for 3-Way Handshake
  void sendSyn(uint16_t msgId, const SynMetadata &syn);
  void sendSynAck(uint16_t msgId, const SynAckMetadata &synAck);
  void sendConnNack(uint16_t msgId, ConnNackReason reason);
  bool waitForSynAck(uint16_t msgId, uint32_t timeoutMs, SynAckMetadata &synAckOut);
  bool waitForConnAck(uint16_t msgId, uint32_t timeoutMs);

  // Internal helper methods for transmission
  bool sendUnreliable(const std::vector<Packet> &packets);
  bool sendReliable(const std::vector<Packet> &packets);
  bool waitForSack(uint16_t msgId, uint32_t timeoutMs, std::vector<uint8_t> &sackBitmapOut);
  bool retransmitMissingChunks(const std::vector<Packet> &packets, const std::vector<uint8_t> &missingIndices);
  uint32_t calculateAckTimeoutMs(size_t totalChunks) const;
  int transmitPacket(const Packet &packet, const char *logPrefix);

  // Internal helper methods for reception
  void sendSACK(uint16_t messageId, uint8_t totalChunks, bool allReceived);
  std::optional<Packet> tryReceivePacket();
  void handleIncomingPacket(const Packet &packet, uint32_t currentTimestampMs);
};