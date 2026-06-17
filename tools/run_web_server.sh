#!/bin/bash
# Directory of this script
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=========================================================="
echo " Starting High-Performance Telemetry Web Server..."
echo " Opening browser or visit: http://localhost:8500"
echo "=========================================================="
python3 "$DIR/web_server.py" "$@"
