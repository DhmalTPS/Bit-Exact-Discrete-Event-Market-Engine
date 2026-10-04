#!/bin/bash
cd "/mnt/f/#Project/Agenthon_T3_Simulation"
g++ -O2 -std=c++17 -march=x86-64-v2 -ffp-contract=off -w -o ~/t3w/cur engine/src/main.cpp -lpthread || exit 1
for r in 1 2 3 4 5; do T3_PROFILE=1 ~/t3w/cur simulate --config ~/t3w/cfg/t3-s001-price-time-priority.json --out ~/t3w/o/s/trace.parquet 2>&1 >/dev/null; done
strace -c -f ~/t3w/cur simulate --config ~/t3w/cfg/t3-s001-price-time-priority.json --out ~/t3w/o/s/trace.parquet 2>&1 >/dev/null | head -12
