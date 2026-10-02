#!/bin/bash
cd "/mnt/f/#Project/Agenthon_T3_Simulation"
g++ -O2 -std=c++17 -march=x86-64-v2 -ffp-contract=off -Wno-missing-field-initializers -o /tmp/t3sim engine/src/main.cpp -lpthread || exit 1
U=upstream/track3-simulation-public/units
for n in t3-s001-price-time-priority t3-eq-uniform-tight t3-fastlob-core t3-mp05-cancel-churn-newest t3-mr-deep-book-state-size t3-gb-highfreq-40hz-60s t3-gb-mega-throughput; do
  printf "%-32s " $n; T3_PROFILE=1 /tmp/t3sim simulate --config $U/$n/scenario.json --out /tmp/o/$n/trace.parquet 2>&1 >/dev/null
done
