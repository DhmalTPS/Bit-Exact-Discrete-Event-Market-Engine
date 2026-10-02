"""Exact differential check: t3sim outputs (out/) vs shipped references; also sha + events checks."""
import hashlib, json, pathlib, sys
import pandas as pd
ROOT = pathlib.Path(__file__).parents[2]
U = ROOT / "upstream/track3-simulation-public/units"
OUT = ROOT / "out"
ok = bad = 0
for u in sorted(U.iterdir()):
    cases = [(u.name, u, OUT / u.name)] if (u / "scenario.json").exists() else \
        [(f"{u.name}/{s.stem}", u / "checks/reference_data" / s.stem, OUT / u.name / s.stem) for s in sorted((u / "scenarios").glob("*.json"))]
    for name, ref, cand in cases:
        msgs = []
        c, r = pd.read_parquet(cand / "trace.parquet"), pd.read_parquet(ref / "trace.parquet")
        if not c.equals(r): msgs.append("trace")
        cm, rm = pd.read_parquet(cand / "message_trace.parquet"), pd.read_parquet(ref / "message_trace.parquet")
        if not cm.equals(rm): msgs.append("ledger")
        ev = json.loads((cand / "events.json").read_text())
        if ev["trace_sha256"] != hashlib.sha256((cand / "trace.parquet").read_bytes()).hexdigest(): msgs.append("sha")
        if ev["n_events"] != len(c): msgs.append("n_events")
        if msgs: bad += 1; print("FAIL", name, msgs)
        else: ok += 1
print(f"{ok} exact, {bad} failing")
sys.exit(1 if bad else 0)
