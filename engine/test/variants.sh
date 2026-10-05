#!/bin/bash
# race rc2 binary vs source variants V1, V2 on a representative unit set (min of 5 wall each)
cd "/mnt/f/#Project/Agenthon_T3_Simulation"
for v in v1 v2; do g++ -O2 -std=c++17 -march=x86-64-v2 -ffp-contract=off -w -o ~/t3w/$v ../t3_$v/main.cpp -lpthread || exit 1; done
U=upstream/track3-simulation-public/units
for n in t3-s001-price-time-priority t3-eq-uniform-tight t3-as01-base-mix t3-ca-fast-mean-reversion t3-sf-02-heavy-jump-tails t3-as06-throughput-fast t3-sf-05-deep-book-liquidity t3-mp05-cancel-churn-newest t3-gb-mega-throughput; do
  args="simulate --config $HOME/t3w/all/$n.json --out $HOME/t3w/o/v/trace.parquet"
  declare -A m; for b in rc2 v1 v2; do m[$b]=9; done
  for r in 1 2 3 4 5 6; do for b in rc2 v1 v2; do
    x=$(~/t3w/$b $args | grep -o '"wall_clock_sec": [0-9.e-]*' | awk '{print $2}')
    m[$b]=$(awk -v p=${m[$b]} -v q=$x 'BEGIN{print (q<p)?q:p}')
  done; done
  awk -v n=$n -v a=${m[rc2]} -v b=${m[v1]} -v c=${m[v2]} 'BEGIN{printf "%-32s rc2=%7.2fms  v1=%7.2fms (x%.3f)  v2=%7.2fms (x%.3f)\n", n, a*1e3, b*1e3, b/a, c*1e3, c/a}'
done
