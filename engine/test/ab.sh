#!/bin/bash
# A/B two source trees: build both, interleave 5 runs per unit, report the minimum of
# METRIC=sim (simulation loop, from T3_PROFILE) or METRIC=wall (whole run, from stdout JSON).
A=$1; B=$2; shift 2
METRIC=${METRIC:-sim}
cd "/mnt/f/#Project/Agenthon_T3_Simulation"
g++ -O2 -std=c++17 -march=x86-64-v2 -ffp-contract=off -w -o ~/t3w/A "$A/main.cpp" -lpthread || exit 1
g++ -O2 -std=c++17 -march=x86-64-v2 -ffp-contract=off -w -o ~/t3w/B "$B/main.cpp" -lpthread || exit 1
one() {  # binary -> metric value
  if [ "$METRIC" = wall ]; then
    $1 simulate --config ~/t3w/cfg/$2.json --out ~/t3w/o/$3/trace.parquet | sed 's/.*"wall_clock_sec": \([0-9.e-]*\).*/\1/'
  else
    T3_PROFILE=1 $1 simulate --config ~/t3w/cfg/$2.json --out ~/t3w/o/$3/trace.parquet 2>&1 >/dev/null | sed 's/.* sim=\([0-9.]*\).*/\1/'
  fi
}
for n in ${@:-t3-fastlob-core t3-mr-deep-book-state-size t3-gb-mega-throughput}; do
  ra=9; rb=9
  for r in 1 2 3 4 5; do
    a=$(one ~/t3w/A $n a); b=$(one ~/t3w/B $n b)
    ra=$(echo "$ra $a" | awk '{print ($2<$1)?$2:$1}'); rb=$(echo "$rb $b" | awk '{print ($2<$1)?$2:$1}')
  done
  echo "$n $METRIC A=$ra B=$rb"
done
