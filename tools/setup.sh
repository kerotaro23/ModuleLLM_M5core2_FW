#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
voice_venv="${VOICE_VENV:-$PWD/.venv}"
python3 -m venv "$voice_venv"
"$voice_venv/bin/python" -m pip install --disable-pip-version-check platformio==6.1.18
export PLATFORMIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$PWD/.build/platformio}"
if [[ "${1:-}" == --upstream ]]; then
  "$voice_venv/bin/python" tools/bootstrap_cloud.py
fi
"$voice_venv/bin/pio" run -e core2 -j 4
bash tools/test.sh
