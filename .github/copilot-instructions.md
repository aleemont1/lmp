## Quick orientation — LoRaMultiPacket

This repo is a small PlatformIO/Arduino library that structures multi-chunk LoRa packets.
Keep these facts in mind when editing or extending the code:

- Library layout: `lib/LoRaMultiPacket/src` contains the public headers and implementation: `Packet.hpp`, `Packet.cpp`, `PacketSerializer.hpp`, `PacketSerializer.cpp`.
- Example application entrypoint: `src/main.cpp` (minimal Arduino sketch). The project builds with PlatformIO using the `heltec_wifi_lora_32_V3` environment defined in `platformio.ini`.

## Big picture

- Responsibility: the library defines binary packet layout (packed C++ structs), CRC calculation, and a simple serializer. It does NOT implement radio transmit/receive — that belongs in application code which should use the serializer and constants here.
- Data flow: application -> build Packet (fill `Packet::header` + `Packet::payload`) -> call `Packet::calculateCRC()` -> `PacketSerializer::serialize()` copies header+payload+crc into a byte buffer ready to send via LoRa.
- Size constraints are important: constants in `Packet.hpp` control max sizes (e.g. `MAX_PACKET_SIZE`, `MAX_TX_PACKET_SIZE`, `LORA_MAX_PAYLOAD_SIZE`). Respect these when building payloads.

## Files & key symbols to reference

- `lib/LoRaMultiPacket/src/Packet.hpp`
  - `PacketHeader` (fields: `packetNumber`, `totalChunks`, `chunkIndex`, `payloadSize`, `timestamp`, `protocolVersion`)
  - `Packet` struct methods: `calculateCRC()`, `printPacket()`
  - Constants: `HEADER_SIZE`, `LORA_MAX_PAYLOAD_SIZE` (used to size payload buffer)
- `lib/LoRaMultiPacket/src/Packet.cpp`
  - CRC implementation uses `offsetof(Packet, crc)` to exclude the `crc` field from the CRC calculation (important detail when changing fields).
  - `printPacket()` uses `Serial` for debugging; ensure `Serial.begin()` is called by the sketch before printing.
  - NOTE: `printPacket()` currently references `chunkNumber` and `chunkSize` — these names do not match the fields in `PacketHeader` (`chunkIndex`/`payloadSize`). Search for this mismatch before editing; it's a discovered inconsistency to fix.
- `lib/LoRaMultiPacket/src/PacketSerializer.cpp`
  - `PacketSerializer::serialize()` performs three `memcpy()` calls: header, payload, crc. It assumes the packed layout defined in `Packet.hpp`.

## Build / test / debugging (practical commands)

Use PlatformIO (CLI or VSCode extension). Typical commands (from repository root):

```bash
# build the default environment
pio run -e heltec_wifi_lora_32_V3

# upload to device (wired)
pio run -e heltec_wifi_lora_32_V3 -t upload

# open serial monitor (adjust baud in your sketch)
pio device monitor -e heltec_wifi_lora_32_V3
```

If using the VSCode PlatformIO extension, open the project and use the UI tasks (Build, Upload, Monitor).

## Project-specific conventions & gotchas

- Packed structs: `#pragma pack(push, 1)` / `#pragma pack(pop)` are used in `Packet.hpp`. Adding/removing fields changes raw layout and CRC — always update `calculateCRC()` expectations and `HEADER_SIZE` if you change `PacketHeader`.
- CRC calculation: implemented in `Packet::calculateCRC()` using a CRC-16 loop and `offsetof(Packet, crc)`. Any layout or alignment change must preserve this behavior.
- Serialization: `PacketSerializer::serialize()` assumes fixed-size payload copy using `sizeof(PacketPayload)`. When changing `LORA_MAX_PAYLOAD_SIZE` or payload semantics, update both the header constants and serializer.
- Arduino runtime assumptions: debug printing uses `Serial` and `String` (Arduino core). Make sure `Serial.begin()` is present in `setup()` for readable output.

## Useful quick searches for automation tasks

- Find where packet layout/size is referenced: `grep -R "LORA_MAX_PAYLOAD_SIZE\|HEADER_SIZE\|PacketHeader" -n`
- Find CRC code: `grep -R "calculateCRC" -n`
- Find serializer usage: `grep -R "PacketSerializer" -n`

## Examples to copy/paste for an agent

- Build a buffer and serialize a packet (pseudo-code referencing repo symbols):

```cpp
Packet p;
p.header.packetNumber = 1;
p.header.totalChunks = 2;
p.header.chunkIndex = 0;
p.header.payloadSize = /* <= LORA_MAX_PAYLOAD_SIZE */;
// fill p.payload.data[0..p.header.payloadSize-1]
p.calculateCRC();
uint8_t buffer[HEADER_SIZE + sizeof(PacketPayload) + sizeof(uint16_t)];
PacketSerializer::serialize(p, buffer);
// send `buffer` using LoRa radio API
```

## When making edits, prioritise

1. Preserve binary layout compatibility unless intentionally bumping protocol version. The `protocolVersion` field exists for that purpose.
2. Update `calculateCRC()` and `PacketSerializer` together with any layout change.
3. Run a build (`pio run`) and a serial test (`pio device monitor`) after changes that affect runtime behavior.

## Reporting issues for other agents / maintainers

- Mention the file and exact symbol names (e.g., `Packet::printPacket()` in `lib/LoRaMultiPacket/src/Packet.cpp`) and include small diffs if proposing fixes.

---

If any of the above assumptions are wrong or you want me to expand this with examples (unit tests, CI, or a simple transmitter/receiver example), tell me which area to expand.
