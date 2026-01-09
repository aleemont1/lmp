#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/.unit_test_build"
mkdir -p "$BUILD_DIR"

g++ -std=c++17 \
  -I"$ROOT_DIR/components/LoRaMultiPacket/include" \
  "$ROOT_DIR/components/LoRaMultiPacket/src/PacketSerializer.cpp" \
  "$ROOT_DIR/unit_test/packet_impl.cpp" \
  "$ROOT_DIR/unit_test/main.cpp" \
  -O2 -o "$BUILD_DIR/run_unit_tests"

echo "Running unit tests..."
"$BUILD_DIR/run_unit_tests"
