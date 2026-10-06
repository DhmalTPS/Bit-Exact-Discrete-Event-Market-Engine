#!/bin/bash
S="/mnt/f/#Project/Agenthon_T3_Simulation/build/scratch"; mkdir -p $S/cfg $S/o $S/all  # in-project scratch only
# Race the exact rc2 and rc3 image binaries on every public unit (incl. batch), interleaved,
# min wall of 5 runs each; prints unit rows rc2_ms rc3_ms
cd "/mnt/f/#Project/Agenthon_T3_Simulation"
cp build/rc/rc2 build/rc/rc3 $S/ && chmod +x $S/rc2 $S/rc3
U=upstream/track3-simulation-public/units
mkdir -p $S/all
wall() { $1 $2 2>/dev/null | grep -o '"wall_clock_sec": [0-9.e-]*' | awk '{print $2}'; }
for u in $U/*/; do
  n=$(basename $u)
  if [ -f $u/scenario.json ]; then
    cp $u/scenario.json $S/all/$n.json
    args="simulate --config $S/all/$n.json --out $S/o/ab/trace.parquet"
    rows=$($S/rc3 $args | grep -o '"n_events": [0-9]*' | awk '{print $2}')
    pat='"wall_clock_sec": [0-9.e-]*'
  else
    rm -rf $S/all/$n; cp -r $u/scenarios $S/all/$n
    args="simulate-batch --batch-dir $S/all/$n --out-dir $S/o/abb"
    rows=$($S/rc3 $args | grep -o '"total_events": [0-9]*' | awk '{print $2}')
  fi
  a=9; b=9
  for r in 1 2 3 4 5; do
    x=$($S/rc2 $args | grep -o '"wall_clock_sec": [0-9.e-]*' | awk '{print $2}')
    y=$($S/rc3 $args | grep -o '"wall_clock_sec": [0-9.e-]*' | awk '{print $2}')
    a=$(awk -v p=$a -v q=$x 'BEGIN{print (q<p)?q:p}'); b=$(awk -v p=$b -v q=$y 'BEGIN{print (q<p)?q:p}')
  done
  echo "$n $rows $a $b"
done
