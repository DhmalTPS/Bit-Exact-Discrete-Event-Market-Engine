#!/bin/bash
# rebuild build/t3sim and run every public case (outputs under out/)
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
g++ -O2 -std=c++17 -march=x86-64-v2 -ffp-contract=off -Wall -Wno-missing-field-initializers -o build/t3sim engine/src/main.cpp -lpthread 2>&1 | grep -E "error|warning" | head
bash engine/test/run_all.sh
