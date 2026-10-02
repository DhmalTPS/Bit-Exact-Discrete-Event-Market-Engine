#!/bin/bash
# usage: bench.sh "<g++ flags>" [units...]  -> min-of-3 phase times per unit
cd "/mnt/f/#Project/Agenthon_T3_Simulation"; mkdir -p $HOME/t3w
g++ $1 -std=c++17 -ffp-contract=off -Wno-missing-field-initializers -o $HOME/t3w/t3b engine/src/main.cpp -lpthread || exit 1
U=upstream/track3-simulation-public/units
shift
for n in "${@:-t3-fastlob-core t3-mr-deep-book-state-size t3-gb-mega-throughput}"; do
 for n2 in $n; do
  mkdir -p $HOME/t3w/cfg; cp $U/$n2/scenario.json $HOME/t3w/cfg/$n2.json
  for r in 1 2 3; do T3_PROFILE=1 $HOME/t3w/t3b simulate --config $HOME/t3w/cfg/$n2.json --out $HOME/t3w/o/$n2/trace.parquet 2>&1 >/dev/null; done | awk -v n=$n2 '{for(i=1;i<=NF;i++){split($i,a,"="); if(a[1]=="sim"||a[1]=="encode"||a[1]=="write"||a[1]=="parse"){v=a[2]+0; if(!(a[1] in m)||v<m[a[1]])m[a[1]]=v}}} END{printf "%-30s parse=%.4f sim=%.4f encode=%.4f write=%.4f\n",n,m["parse"],m["sim"],m["encode"],m["write"]}'
 done
done
if [ -n "$TIMEV" ]; then /usr/bin/time -v $HOME/t3w/t3b simulate --config $HOME/t3w/cfg/t3-gb-mega-throughput.json --out $HOME/t3w/o/m/trace.parquet 2>&1 | egrep 'Elapsed|User time|System time|Maximum resident|Minor'; fi
