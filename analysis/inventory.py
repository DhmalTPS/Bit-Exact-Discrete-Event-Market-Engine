"""Inventory every public unit: family, tier, ledger flag, agent mix, latency, oracle, events, rate."""
import json, pathlib, tomllib, collections
import pyarrow.parquet as pq

ROOT = pathlib.Path("upstream/track3-simulation-public/units")
rows = []
for u in sorted(ROOT.iterdir()):
    card = tomllib.loads((u / "card.toml").read_text())
    params = card["scoring"]["params"]
    fam = card["task"]["scenario_family"]
    if (u / "batch.json").exists():
        b = json.loads((u / "batch.json").read_text())
        subs = sorted((u / "scenarios").glob("*.json"))
        rows.append(dict(unit=u.name, fam=fam, batch=len(subs), tier=params.get("semantic_tier"),
                         ledger=params.get("requires_message_ledger")))
        continue
    s = json.loads((u / "scenario.json").read_text())
    ev = json.loads((u / "events.json").read_text())
    mix = ",".join(f"{a['agent_type'][:2]}{a['count']}" for a in s["agent_configs"])
    lat = s.get("latency_config", {})
    op = s["oracle_config"].get("params", {})
    ex = s["exchange_config"]
    rows.append(dict(unit=u.name, fam=fam, tier=params.get("semantic_tier"), ledger=params.get("requires_message_ledger"),
        mix=mix, n_agents=sum(a["count"] for a in s["agent_configs"]), horizon_s=s["horizon_ns"]/1e9,
        lat=lat.get("model"), oracle=s["oracle_config"].get("type"), jump=op.get("jump_intensity"),
        sj=bool(op.get("scheduled_jump")), proto=ex.get("protocol_enforcement", False), stp=ex.get("stp_policy"),
        n_events=ev["n_events"], n_msgs=ev.get("n_messages"), eps=round(ev["events_per_sec"]), wall=round(ev["wall_clock_sec"],2)))
cols = ["unit","fam","tier","ledger","mix","n_agents","horizon_s","lat","jump","sj","proto","stp","n_events","n_msgs","eps","wall","batch"]
print("\t".join(cols))
for r in rows:
    print("\t".join(str(r.get(c,"")) for c in cols))
json.dump(rows, open("analysis/inventory.json","w"), indent=1)
tot = sum(r.get("n_events",0) for r in rows)
print("total events", tot, "families", collections.Counter(r["fam"] for r in rows))
