#!/bin/bash
S="/mnt/f/#Project/Agenthon_T3_Simulation/build/scratch"; mkdir -p $S/cfg $S/o $S/all  # in-project scratch only
cd "/mnt/f/#Project/Agenthon_T3_Simulation"
g++ -O2 -std=c++17 -march=x86-64-v2 -ffp-contract=off -w -o $S/cur engine/src/main.cpp -lpthread || exit 1
for r in 1 2 3 4 5; do T3_PROFILE=1 $S/cur simulate --config $S/cfg/t3-s001-price-time-priority.json --out $S/o/s/trace.parquet 2>&1 >/dev/null; done
strace -c -f $S/cur simulate --config $S/cfg/t3-s001-price-time-priority.json --out $S/o/s/trace.parquet 2>&1 >/dev/null | head -12
