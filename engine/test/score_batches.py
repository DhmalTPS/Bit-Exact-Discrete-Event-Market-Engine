"""Run every public batch unit through the image's simulate-batch verb and score it with the
official qfbench2_track_simulation.batch gates (aggregate schema + per-sub isolation)."""
import json, pathlib, subprocess, sys, tomllib
from qfbench2_track_simulation import batch, scoring
ROOT = pathlib.Path(__file__).parents[2]
U = ROOT / "upstream/track3-simulation-public/units"
img = sys.argv[1]
allok = True
for u in sorted(U.glob("t3-gbatch-*")):
    out = ROOT / "run_outputs" / "batch" / u.name
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run(["docker", "run", "--rm", "--network", "none", "--cpus", "4", "--memory", "16g",
                    "-v", f"{u / 'scenarios'}:/input/scenarios:ro", "-v", f"{out}:/output", img,
                    "simulate-batch", "--batch-dir", "/input/scenarios", "--out-dir", "/output"],
                   check=True, capture_output=True)
    subs = batch.load_subs(u)
    ok1, info = batch.check_aggregate(out, subs)
    pol = scoring._CardPolicy(u)
    ok2, fails = batch.score_isolation(u, out, pol.family, pol.tier, pol.ceilings)
    be = json.loads((out / "batch_events.json").read_text())
    print(f"{u.name:24s} aggregate={ok1} isolation={ok2} total={be['total_events']} eps={be['events_per_sec']:.0f} {fails[:1]}")
    allok &= ok1 and ok2
print("ALL PASS" if allok else "FAILURES")
