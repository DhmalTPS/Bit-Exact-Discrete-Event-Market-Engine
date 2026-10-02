#!/bin/bash
# Build a PGO binary: instrument, train on a spread of public units, rebuild with the profile.
set -e
cd "/mnt/f/#Project/Agenthon_T3_Simulation"
OUT=${1:-$HOME/t3w/t3pgo}
P=$HOME/t3w/pgo; rm -rf $P; mkdir -p $P
FLAGS="-O2 -std=c++17 -march=x86-64-v2 -ffp-contract=off -w"
g++ $FLAGS -fprofile-generate -fprofile-dir=$P -o $HOME/t3w/t3inst engine/src/main.cpp -lpthread
U=upstream/track3-simulation-public/units
for n in t3-fastlob-core t3-mp02-stp-oldest-baseline t3-mr-deep-book-state-size t3-gb-pop-128-agents t3-ra01-fundamental-shock-mid t3-sf-02-heavy-jump-tails t3-eq-pareto-heavytail t3-eq-deterministic-baseline; do
  $HOME/t3w/t3inst simulate --config $U/$n/scenario.json --out $HOME/t3w/o/pgo/trace.parquet > /dev/null
done
g++ $FLAGS -fprofile-use -fprofile-dir=$P -fprofile-correction -o $OUT engine/src/main.cpp -lpthread
echo built $OUT
