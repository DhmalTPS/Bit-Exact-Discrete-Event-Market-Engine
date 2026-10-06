#!/bin/bash
S="/mnt/f/#Project/Agenthon_T3_Simulation/build/scratch"; mkdir -p $S/cfg $S/o $S/all  # in-project scratch only
cd "/mnt/f/#Project/Agenthon_T3_Simulation"
g++ -O2 -DT3_PROF -std=c++17 -march=x86-64-v2 -ffp-contract=off -Wno-missing-field-initializers -o $S/t3p engine/src/main.cpp -lpthread || exit 1
for n in t3-fastlob-core t3-gb-mega-throughput; do echo $n; $S/t3p simulate --config $S/cfg/$n.json --out $S/o/p/trace.parquet > /dev/null; done
