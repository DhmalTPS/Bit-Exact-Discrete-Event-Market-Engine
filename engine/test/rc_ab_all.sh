#!/bin/bash
# Race the exact rc2 and rc3 image binaries on every public unit (incl. batch), interleaved,
# min wall of 5 runs each; prints unit rows rc2_ms rc3_ms
cd "/mnt/f/#Project/Agenthon_T3_Simulation"
cp build/rc/rc2 build/rc/rc3 ~/t3w/ && chmod +x ~/t3w/rc2 ~/t3w/rc3
U=upstream/track3-simulation-public/units
mkdir -p ~/t3w/all
wall() { $1 $2 2>/dev/null | grep -o '"wall_clock_sec": [0-9.e-]*' | awk '{print $2}'; }
for u in $U/*/; do
  n=$(basename $u)
  if [ -f $u/scenario.json ]; then
    cp $u/scenario.json ~/t3w/all/$n.json
    args="simulate --config $HOME/t3w/all/$n.json --out $HOME/t3w/o/ab/trace.parquet"
    rows=$(~/t3w/rc3 $args | grep -o '"n_events": [0-9]*' | awk '{print $2}')
    pat='"wall_clock_sec": [0-9.e-]*'
  else
    rm -rf ~/t3w/all/$n; cp -r $u/scenarios ~/t3w/all/$n
    args="simulate-batch --batch-dir $HOME/t3w/all/$n --out-dir $HOME/t3w/o/abb"
    rows=$(~/t3w/rc3 $args | grep -o '"total_events": [0-9]*' | awk '{print $2}')
  fi
  a=9; b=9
  for r in 1 2 3 4 5; do
    x=$(~/t3w/rc2 $args | grep -o '"wall_clock_sec": [0-9.e-]*' | awk '{print $2}')
    y=$(~/t3w/rc3 $args | grep -o '"wall_clock_sec": [0-9.e-]*' | awk '{print $2}')
    a=$(awk -v p=$a -v q=$x 'BEGIN{print (q<p)?q:p}'); b=$(awk -v p=$b -v q=$y 'BEGIN{print (q<p)?q:p}')
  done
  echo "$n $rows $a $b"
done
