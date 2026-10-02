#!/bin/bash
cd "/mnt/f/#Project/Agenthon_T3_Simulation"
g++ -O2 -DT3_PROF -std=c++17 -march=x86-64-v2 -ffp-contract=off -Wno-missing-field-initializers -o ~/t3w/t3p engine/src/main.cpp -lpthread || exit 1
for n in t3-fastlob-core t3-gb-mega-throughput; do echo $n; ~/t3w/t3p simulate --config ~/t3w/cfg/$n.json --out ~/t3w/o/p/trace.parquet > /dev/null; done
