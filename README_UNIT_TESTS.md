# Unit tests (host)

This project includes a small host-based unit test harness that exercises
core logic (CRC and packet splitting) without any external hardware or
ESP-IDF dependencies.

How to run

From the repo root:

```bash
./tools/run_unit_tests.sh
```

Requirements
- A host C++ compiler (g++ supporting C++17)

What it tests
- `Packet::calculateCRC()` behaviour
- `PacketSerializer::splitVectorToPackets()` and reassembly correctness

If you want tests integrated with CI, I can add a GitHub Actions workflow
that runs the test script on push/PR.
