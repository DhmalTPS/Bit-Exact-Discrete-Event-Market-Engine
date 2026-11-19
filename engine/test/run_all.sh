#!/bin/bash
# Run t3sim on every public single unit and every batch sub; outputs under out/<case>/
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
U=upstream/track3-simulation-public/units
for u in $U/*/; do
  n=$(basename $u)
  if [ -f $u/scenario.json ]; then
    ./build/t3sim simulate --config $u/scenario.json --out out/$n/trace.parquet > /dev/null
  else
    for s in $u/scenarios/*.json; do
      ./build/t3sim simulate --config $s --out out/$n/$(basename $s .json)/trace.parquet > /dev/null
    done
  fi
done
echo done
