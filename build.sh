#!/bin/sh
# Builds prep.wasm from src/prep.cpp. Needs clang with the wasm32 target and wasm-ld (no Emscripten, no libc).
set -e
cd "$(dirname "$0")"
clang --target=wasm32 -O3 -ffreestanding -nostdlib -fno-exceptions -fno-rtti -std=c++17 -Wall \
  -Wl,--no-entry -Wl,--export-dynamic -Wl,-z,stack-size=262144 -o prep.wasm src/prep.cpp
echo "prep.wasm: $(wc -c < prep.wasm) bytes"
