#!/bin/bash
# A/B two prebuilt binaries, interleaved, min sim time of 5 runs per unit
A=$1; B=$2; shift 2
for n in ${@:-t3-fastlob-core t3-mr-deep-book-state-size t3-gb-mega-throughput}; do
  ra=9; rb=9
  for r in 1 2 3 4 5; do
    a=$(T3_PROFILE=1 $A simulate --config ~/t3w/cfg/$n.json --out ~/t3w/o/a/trace.parquet 2>&1 >/dev/null | sed 's/.*sim=\([0-9.]*\).*/\1/')
    b=$(T3_PROFILE=1 $B simulate --config ~/t3w/cfg/$n.json --out ~/t3w/o/b/trace.parquet 2>&1 >/dev/null | sed 's/.*sim=\([0-9.]*\).*/\1/')
    ra=$(echo "$ra $a" | awk '{print ($2<$1)?$2:$1}'); rb=$(echo "$rb $b" | awk '{print ($2<$1)?$2:$1}')
  done
  echo "$n A=$ra B=$rb"
done
