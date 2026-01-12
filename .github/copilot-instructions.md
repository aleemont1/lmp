## Quick orientation — LoRaMultiPacket

This repo is a small PlatformIO/Arduino library that structures multi-chunk LoRa packets.
Keep these facts in mind when editing or extending the code:

- Library layout: `lib/LoRaMultiPacket/src` contains the public headers and implementation: `Packet.hpp`, `Packet.cpp`, `PacketSerializer.hpp`, `PacketSerializer.cpp`.
- Example application entrypoint: `src/main.cpp` (minimal Arduino sketch). The project builds with PlatformIO using the `heltec_wifi_lora_32_V3` environment defined in `platformio.ini`.

## Big picture

- Responsibility: the library defines binary packet layout (packed C++ structs), CRC calculation, and a simple serializer. It does NOT implement radio transmit/receive — that belongs in application code which should use the serializer and constants here.
- Data flow: application -> build Packet (fill `Packet::header` + `Packet::payload`) -> call `Packet::calculateCRC()` -> `PacketSerializer::serialize()` copies header+payload+crc into a byte buffer ready to send via LoRa.
- Size constraints are important: constants in `Packet.hpp` control max sizes (e.g. `MAX_PACKET_SIZE`, `MAX_TX_PACKET_SIZE`, `LORA_MAX_PAYLOAD_SIZE`). Respect these when building payloads.

````instructions
## Quick orientation — LoRaMultiPacket

This repo implements a compact, packed C++ library that structures multi-chunk LoRa packets and a minimal example application. Use these notes when editing or extending the code.

- Library layout (current): `components/LoRaMultiPacket`
  - Public headers: `components/LoRaMultiPacket/include/Packet.hpp`, `components/LoRaMultiPacket/include/PacketSerializer.hpp`
  - Implementation sources: `components/LoRaMultiPacket/src/Packet.cpp`, `components/LoRaMultiPacket/src/PacketSerializer.cpp`
- Example application entrypoint: `src/main.cpp` (minimal Arduino/PlatformIO sketch). The project builds with PlatformIO; an environment named `heltec_wifi_lora_32_V3` is defined in `platformio.ini`.

## Big picture

- Responsibility: the package defines the binary packet layout (packed structs), CRC calculation, and a serializer. It intentionally does not perform radio TX/RX — application code should call the serializer then hand the buffer to the radio API.
- Data flow: application -> populate `Packet` (fill `Packet::header` + `Packet::payload`) -> call `Packet::calculateCRC()` -> `PacketSerializer::serialize()` copies header, fixed-size payload region, and CRC into a send buffer.
- Size constraints are important: constants in `Packet.hpp` control sizes (for example `HEADER_SIZE`, `LORA_MAX_PAYLOAD_SIZE`). Respect those when constructing `payloadSize` and sending packets.

## Files & key symbols

- `components/LoRaMultiPacket/include/Packet.hpp`
  - `PacketHeader` fields: `packetNumber`, `totalChunks`, `chunkIndex`, `payloadSize`, `timestamp`, `protocolVersion` (verify exact names in the header before changes).
  - `Packet` methods: `calculateCRC()`, `printPacket()`.
  - Size constants: `HEADER_SIZE`, `LORA_MAX_PAYLOAD_SIZE` (used to size payload buffers and for serialization).
- `components/LoRaMultiPacket/src/Packet.cpp`
  - `Packet::calculateCRC()` computes CRC-16 over the packet up to (but excluding) the `crc` field using `offsetof(Packet, crc)`. When modifying the packet layout, keep this behavior and update any size constants.
  - `Packet::printPacket()` uses `Serial` for debug output; ensure `Serial.begin()` is called by the sketch before printing. Note: there is a past mismatch where `printPacket()` referenced `chunkNumber`/`chunkSize` while the header uses `chunkIndex`/`payloadSize` — search and fix name mismatches if you touch debug code.
- `components/LoRaMultiPacket/src/PacketSerializer.cpp`
  - `PacketSerializer::serialize()` does sequential `memcpy()` operations: header, payload region (fixed size), then CRC. It relies on the packed layout and the payload buffer size defined in `Packet.hpp`.

## Build / test / debugging (practical commands)

Use PlatformIO (CLI or VSCode extension). From repository root:

```bash
# build the default environment
pio run -e heltec_wifi_lora_32_V3

# upload to device (wired)
pio run -e heltec_wifi_lora_32_V3 -t upload

# open serial monitor (adjust baud in your sketch)
pio device monitor -e heltec_wifi_lora_32_V3
```

Unit tests and helper code lives in `test/` and `unit_test/` — use `tools/run_unit_tests.sh` to run hosted tests where available.

## Project-specific conventions & gotchas

- Packed layout: the headers use `#pragma pack(push, 1)` / `#pragma pack(pop)` to ensure a compact binary layout. Adding or removing fields changes the raw on-wire layout and will change CRC inputs; if you change fields, update `HEADER_SIZE`, any consumer code, and document the protocol version bump.
- CRC: `Packet::calculateCRC()` uses a CRC-16 loop and `offsetof(Packet, crc)` so the `crc` field itself is excluded from the calculation. Preserve this if you change the struct order or add fields.
- Serialization: `PacketSerializer::serialize()` assumes a fixed payload buffer region sized with `LORA_MAX_PAYLOAD_SIZE` (see the header). If you change `LORA_MAX_PAYLOAD_SIZE`, update both the header and the serializer to match.
- Debug printing: `Packet::printPacket()` prints to `Serial`. Confirm `Serial.begin()` is present in `src/main.cpp` when using prints on a device.

## Quick searches

To find relevant symbols or sizes quickly:

```bash
grep -R "LORA_MAX_PAYLOAD_SIZE\|HEADER_SIZE\|PacketHeader" -n
grep -R "calculateCRC" -n
grep -R "PacketSerializer" -n
```

## Reporting issues / editing tips

- When reporting or changing code, mention exact symbols and file paths — for example `Packet::printPacket()` in [components/LoRaMultiPacket/src/Packet.cpp](components/LoRaMultiPacket/src/Packet.cpp).
- If you change the packet layout, update `Packet::calculateCRC()`, `HEADER_SIZE`, and `PacketSerializer::serialize()` together to keep on-wire compatibility predictable. Use `protocolVersion` in the header when you intentionally change wire format.

---

If you'd like, I can also:
- add a short unit test demonstrating serialization and CRC verification,
- fix the `printPacket()` name mismatches, or
- add a tiny example transmitter/receiver sketch that uses the serializer.
Tell me which and I'll implement it.
````
