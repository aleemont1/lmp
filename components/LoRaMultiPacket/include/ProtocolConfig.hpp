#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @namespace LoRaMultiPacketConfig
 * @brief Centralized configuration constants for the LoRaMultiPacket protocol.
 */
namespace LoRaMultiPacketConfig
{
// Protocol Version
constexpr uint8_t PROTOCOL_VERSION = 2;

// Connection and Handshake Configuration
constexpr uint32_t DEFAULT_SYN_TIMEOUT_MS = 3000;
constexpr uint32_t DEFAULT_CONN_INACTIVITY_TIMEOUT_MS = 15000;

// Retry and Timeout Limits
constexpr int MAX_RETRIES = 5;
constexpr uint32_t MIN_ACK_TIMEOUT_MS = 1000;
constexpr uint32_t PRUNE_TIMEOUT_MS = 15000;

// Delays (in milliseconds)
constexpr uint32_t SACK_PREAMBLE_GUARD_DELAY_MS = 25;
constexpr uint32_t POST_TX_GUARD_DELAY_MS = 5;
constexpr uint32_t POLL_SLEEP_DELAY_MS = 10;

// Session and Reassembly Limits
constexpr size_t MAX_CONCURRENT_SESSIONS = 10;
constexpr size_t MAX_COMPLETED_HISTORY = 16;

// Airtime Safety Multipliers
constexpr double ACK_TIMEOUT_SAFETY_FACTOR = 1.5;
constexpr uint32_t ACK_TIMEOUT_GUARD_MS = 25;

// ---------------------------------------------------------------------------
// Optional MAC/PHY Compliance Helpers
// ---------------------------------------------------------------------------
// These features operate BELOW the LAMP transport layer and are therefore
// DISABLED by default.  Regulatory compliance (ETSI EN 300 220, FCC Part 15,
// etc.) is the responsibility of the MAC/PHY layer or the integrating
// application, not of the transport protocol itself.
//
// Enable them in your application via:
//   protocol.setDutyCyclePacing(true, 0.01f);  // 1% sub-band
//   protocol.setCadEnabled(true);
// ---------------------------------------------------------------------------

// Duty-Cycle Pacing (opt-in, disabled by default)
constexpr float DEFAULT_DUTY_CYCLE_LIMIT = 0.01f;  // reference limit when enabled
constexpr bool DEFAULT_DUTY_CYCLE_PACING_ENABLED = false;

// Channel Activity Detection / LBT (opt-in, disabled by default)
constexpr bool DEFAULT_CAD_ENABLED = false;
constexpr int MAX_CAD_RETRIES = 5;
constexpr uint32_t CAD_BACKOFF_BASE_MS = 20;

// Buffer Sizes
constexpr size_t PHY_BUFFER_SIZE = 256;
}  // namespace LoRaMultiPacketConfig
