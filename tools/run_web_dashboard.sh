#!/bin/bash
# Directory of this script
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Check if virtual environment exists, if not create it
if [ ! -d "$DIR/.venv" ]; then
    echo "Creating Python virtual environment in $DIR/.venv..."
    python3 -m venv "$DIR/.venv"
    if [ $? -ne 0 ]; then
        echo "Error: Failed to create virtual environment. Make sure python3-venv is installed."
        exit 1
    fi
    echo "Upgrading pip..."
    "$DIR/.venv/bin/pip" install --upgrade pip
fi

# Ensure all dependencies from requirements.txt are installed (fast check)
echo "Syncing dependencies in virtual environment..."
"$DIR/.venv/bin/pip" install -q -r "$DIR/requirements.txt"

# Run the streamlit dashboard using the virtual environment interpreter
echo "=========================================================="
echo " Starting Streamlit Web Dashboard..."
echo " Opening browser or visit: http://localhost:8501"
echo "=========================================================="
"$DIR/.venv/bin/streamlit" run "$DIR/web_dashboard.py" "$@"
