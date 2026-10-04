#!/bin/bash
# wall-clock (min of 7) per unit for T3_ASYNC=0 vs 1, plus the estimate-based default
cd "/mnt/f/#Project/Agenthon_T3_Simulation"
g++ -O2 -std=c++17 -march=x86-64-v2 -ffp-contract=off -w -o ~/t3w/cur engine/src/main.cpp -lpthread || exit 1
U=upstream/track3-simulation-public/units
best() { local m=9; for r in 1 2 3 4 5 6 7; do w=$(env $1 ~/t3w/cur simulate --config ~/t3w/cfg/$2.json --out ~/t3w/o/s/trace.parquet | grep -o '"wall_clock_sec": [0-9.e-]*' | awk '{print $2}'); m=$(awk -v a=$m -v b=$w 'BEGIN{print (b<a)?b:a}'); done; echo $m; }
for n in t3-s001-price-time-priority t3-eq-uniform-tight t3-as01-base-mix t3-ca-baseline-calm t3-fastlob-core t3-mp05-cancel-churn-newest t3-gb-mega-throughput; do
  cp $U/$n/scenario.json ~/t3w/cfg/$n.json
  printf "%-32s sync=%8.2fms  async=%8.2fms  default=%8.2fms\n" $n $(awk -v x=$(best T3_ASYNC=0 $n) 'BEGIN{print x*1000}') $(awk -v x=$(best T3_ASYNC=1 $n) 'BEGIN{print x*1000}') $(awk -v x=$(best X=1 $n) 'BEGIN{print x*1000}')
done
