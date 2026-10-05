#!/bin/bash
# re-race rc2 vs current build on named units, 9 interleaved repeats, min wall
cd "/mnt/f/#Project/Agenthon_T3_Simulation"; cp build/rc/rc3 ~/t3w/rc3
for n in "$@"; do
  if [ -d ~/t3w/all/$n ]; then args="simulate-batch --batch-dir $HOME/t3w/all/$n --out-dir $HOME/t3w/o/rb"; else args="simulate --config $HOME/t3w/all/$n.json --out $HOME/t3w/o/r/trace.parquet"; fi
  a=9; b=9
  for r in 1 2 3 4 5 6 7 8 9; do
    x=$(~/t3w/rc2 $args | grep -o '"wall_clock_sec": [0-9.e-]*' | awk '{print $2}'); y=$(~/t3w/rc3 $args | grep -o '"wall_clock_sec": [0-9.e-]*' | awk '{print $2}')
    a=$(awk -v p=$a -v q=$x 'BEGIN{print (q<p)?q:p}'); b=$(awk -v p=$b -v q=$y 'BEGIN{print (q<p)?q:p}')
  done
  awk -v n=$n -v a=$a -v b=$b 'BEGIN{printf "%-32s rc2=%7.2fms new=%7.2fms x%.3f\n", n, a*1e3, b*1e3, b/a}'
done
