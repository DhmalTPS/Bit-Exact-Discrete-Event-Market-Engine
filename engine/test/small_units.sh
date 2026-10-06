#!/bin/bash
S="/mnt/f/#Project/Agenthon_T3_Simulation/build/scratch"; mkdir -p $S/cfg $S/o $S/all  # in-project scratch only
# whole-run wall (stdout JSON) + phase profile for the smallest public units, native ext4
cd "/mnt/f/#Project/Agenthon_T3_Simulation"
g++ -O2 -std=c++17 -march=x86-64-v2 -ffp-contract=off -w -o $S/cur engine/src/main.cpp -lpthread || exit 1
U=upstream/track3-simulation-public/units
for n in t3-s001-price-time-priority t3-eq-deterministic-baseline t3-s019-latency-jitter-kendall t3-eq-uniform-tight t3-as01-base-mix t3-fastlob-core; do
  cp $U/$n/scenario.json $S/cfg/$n.json
  best=9; prof=""
  for r in 1 2 3 4 5 6 7; do
    out=$(T3_PROFILE=1 $S/cur simulate --config $S/cfg/$n.json --out $S/o/s/trace.parquet 2>&1)
    w=$(echo "$out" | grep -o '"wall_clock_sec": [0-9.e-]*' | awk '{print $2}')
    if awk "BEGIN{exit !($w < $best)}"; then best=$w; prof=$(echo "$out" | grep profile); fi
  done
  rows=$(echo "$prof" | sed 's/.*rows=\([0-9]*\).*/\1/')
  awk -v n=$n -v w=$best -v r=$rows 'BEGIN{printf "%-32s rows=%7d wall=%7.3f ms  self-rate=%11.0f\n", n, r, w*1000, r/w}'
  echo "      $prof" | sed 's/profile //'
done
