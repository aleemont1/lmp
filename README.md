<div align="center">


# LoRaMultiPacket Protocol

**LoRaMultiPacket**  
A lightweight, robust C++ library for packet segmentation and reassembly over narrowband LoRa links.

![Unit Tests](https://img.shields.io/github/actions/workflow/status/aleemont1/lmp/ci.yml?style=for-the-badge)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)
![PlatformIO](https://img.shields.io/badge/PlatformIO-f5822a?style=for-the-badge&logo=platformio&logoColor=white)
![Unity](https://img.shields.io/badge/Unity-181717?style=for-the-badge&logo=c&logoColor=white)
![LoRa](https://img.shields.io/badge/LoRa-3399CC?style=for-the-badge&logo=semtech&logoColor=white)

</div>

---

## 📡 Overview

**LoRaMultiPacket** is a lightweight and robust C++ transport-layer library designed to enable packet segmentation and reassembly over narrowband radio links such as LoRa.

Originally developed for the **Borealis experimental rocket telemetry system**  
(Aurora Rocketry – University of Bologna), the library addresses the payload size limitation of typical LoRa transceivers (≈255 bytes) by implementing a minimal-overhead, reliable packetization protocol.

---

## 🚀 Key Features

- **Automatic Segmentation**  
  Splits arbitrarily large payloads into chunks compatible with LoRa hardware  
  (SX127x, SX126x, EByte E220).

- **Hardware Agnostic Design**  
  Core logic is decoupled from radio drivers and can run on:
  - ESP32
  - STM32
  - Desktop hosts (Linux / macOS) for unit testing

- **Robustness**  
  - Modbus CRC-16 for data integrity  
  - Zero-redundancy header design (implicit SOM/EOM indexing)

- **Efficiency-Oriented**  
  - Fixed 7-byte header  
  - Maximizes payload-to-airtime ratio

- **Test-Driven Development**  
  - Complete unit test suite  
  - Native execution via Unity + PlatformIO

---

## 🛠️ Tech Stack
<div align="center">

| Component | Technology |
|----------|------------|
| Language | C++17 |
| Build / Framework | PlatformIO |
| Testing | Unity |
| Radio Technology | LoRa (Semtech-based modules) |
</div>

---

## ⚙️ Protocol Architecture

The protocol encapsulates application data into a compact `Packet` structure optimized for radio transmission.

### Packet Layout (Little Endian)
<div align="center">

```
+-------------------+-----------------------+------------------+
|      HEADER       |        PAYLOAD        |      FOOTER      |
|     (7 Bytes)     |      (Var Size)       |     (2 Bytes)    |
+-------------------+-----------------------+------------------+
```

</div>

---

### Header Fields
<div align="center">

| Field | Type | Description |
|------|------|-------------|
| messageId | uint16_t | Unique identifier for the full message |
| totalChunks | uint8_t | Total number of fragments |
| chunkIndex | uint8_t | Index of the current fragment (0-based) |
| payloadSize | uint8_t | Number of valid payload bytes |
| flags | uint8_t | Control flags (ACK_REQ, ACK, CONN_REQ, CONN_ACK, CONN_NACK) |
| protocolVer | uint8_t | Protocol version |

</div>

**Flags**
- **ACK_REQ** (`0x04`) – Acknowledgement requested (receiver must reply with SACK)  
- **ACK** (`0x08`) – Selective Acknowledgment feedback packet  
- **CONN_REQ** (`0x10`) – Connection Request  
- **CONN_ACK** (`0x20`) – Connection Acknowledged  
- **CONN_NACK** (`0x40`) – Connection Refused  

*Note: Start Of Message (SOM) and End Of Message (EOM) are implicitly derived from `chunkIndex == 0` and `chunkIndex == totalChunks - 1` respectively, eliminating redundant header flags.*

---

## 📦 Installation

The project is structured as a **PlatformIO component**.

### Clone the repository

```bash
git clone https://github.com/your-username/LoRaMultiPacket.git
```

### Integration

- Add the library as an external dependency in `platformio.ini`, or  
- Include the `components/` folder directly in your project

---

## 💻 Usage Example

### Serialization (Transmitter)

```cpp
#include "PacketSerializer.hpp"
#include <vector>

void sendTelemetry() {
    std::vector<uint8_t> largePayload(500, 0xAB);

    uint16_t messageId = 42;
    auto packets = PacketSerializer::splitVectorToPackets(largePayload, messageId);

    for (const auto& pkt : packets) {
        uint8_t buffer[MAX_PACKET_SIZE];

        PacketSerializer::serialize(pkt, buffer);

        LoRaDriver::send(
            buffer,
            HEADER_SIZE + pkt.header.payloadSize + CRC_SIZE
        );
    }
}
```

---

### Deserialization (Receiver)

```cpp
Packet pkt = PacketSerializer::deserialize(receivedBuffer, length);

uint16_t calculatedCRC = pkt.calculateCRC();
if (pkt.crc == calculatedCRC) {
    // Process or reassemble packet
}
```

---

## 🧪 Testing

The project follows a **Test-Driven Development (TDD)** workflow.

Run tests natively:

```bash
pio test -e native
```

---

## 📄 License

This project is distributed under the **MIT License**.  
See the `LICENSE` file for details.

Third-party dependencies retain their respective licenses.

---

## 👥 Authors & Acknowledgments

- **Alessandro Monticelli** – Author & Lead Developer  
- **Prof. Andrea Piroddi** – Supervisor & Scientific Advisor  

Developed as part of the **Borealis Project**  
Aurora Rocketry – University of Bologna
