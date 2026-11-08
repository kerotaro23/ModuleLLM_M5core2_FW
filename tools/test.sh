#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .build
if [[ ! -d .build/ArduinoJson ]]; then
  git clone --quiet --depth 1 --branch v7.3.1 https://github.com/bblanchon/ArduinoJson.git .build/ArduinoJson
fi
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -Ilib/VoiceCore -I.build/ArduinoJson/src tests/session_test.cpp -o .build/session_test
.build/session_test
python3 -m unittest discover -s tests -p 'test_*.py' -v
