#!/bin/bash
# A/B: build engine at path A and B, interleave runs, report min sim time per unit
A=$1; B=$2; shift 2
cd "/mnt/f/#Project/Agenthon_T3_Simulation"
g++ -O2 -std=c++17 -march=x86-64-v2 -ffp-contract=off -w -o ~/t3w/A "$A/main.cpp" -lpthread || exit 1
g++ -O2 -std=c++17 -march=x86-64-v2 -ffp-contract=off -w -o ~/t3w/B "$B/main.cpp" -lpthread || exit 1
for n in ${@:-t3-fastlob-core t3-mr-deep-book-state-size t3-gb-mega-throughput}; do
  for v in A B; do :; done
  ra=9; rb=9
  for r in 1 2 3 4 5; do
    a=$(T3_PROFILE=1 ~/t3w/A simulate --config ~/t3w/cfg/$n.json --out ~/t3w/o/a/trace.parquet 2>&1 >/dev/null | sed 's/.*sim=\([0-9.]*\).*/\1/')
    b=$(T3_PROFILE=1 ~/t3w/B simulate --config ~/t3w/cfg/$n.json --out ~/t3w/o/b/trace.parquet 2>&1 >/dev/null | sed 's/.*sim=\([0-9.]*\).*/\1/')
    ra=$(echo "$ra $a" | awk '{print ($2<$1)?$2:$1}'); rb=$(echo "$rb $b" | awk '{print ($2<$1)?$2:$1}')
  done
  echo "$n A=$ra B=$rb"
done
