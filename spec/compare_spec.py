"""Differential check: spec_sim vs the shipped ABIDES reference traces (exact trace + exact ledger)."""
import json
import pathlib
import sys
import time

import pandas as pd

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from spec_sim import run_scenario  # noqa: E402

UNITS = pathlib.Path(__file__).parents[1] / "upstream/track3-simulation-public/units"
TRACE_COLS = ["t_ns", "agent_id", "msg_type", "side", "price", "size", "order_id"]
MSG_COLS = ["seq", "t_recv_ns", "t_send_ns", "latency_ns", "src_id", "dst_id", "message_id",
            "msg_type", "order_id", "causal_parent"]


def iter_cases(pattern):
    for u in sorted(UNITS.glob(pattern)):
        if (u / "scenario.json").exists():
            yield u.name, u / "scenario.json", u
        elif (u / "batch.json").exists():
            for s in sorted((u / "scenarios").glob("*.json")):
                yield f"{u.name}/{s.stem}", s, u / "checks/reference_data" / s.stem


def first_diff(a: pd.DataFrame, b: pd.DataFrame, cols):
    n = min(len(a), len(b))
    for c in cols:
        x, y = a[c].iloc[:n].astype("object").to_numpy(), b[c].iloc[:n].astype("object").to_numpy()
        def same(p, q):
            if pd.isna(p) or pd.isna(q):
                return pd.isna(p) and pd.isna(q)
            return p == q
        ne = [i for i in range(n) if not same(x[i], y[i])]
        if ne:
            return c, ne[0]
    return (None, n) if len(a) == len(b) else ("len", n)


def main(pattern="*"):
    ok = 0
    bad = []
    for name, scen, refdir in iter_cases(pattern):
        sc = json.loads(scen.read_text())
        t0 = time.perf_counter()
        sim = run_scenario(sc)
        dt = time.perf_counter() - t0
        tr = pd.DataFrame(sim.trace_rows(), columns=TRACE_COLS)
        cols = list(zip(*sim.ledger)) if sim.ledger else [[]] * len(MSG_COLS)
        led = pd.DataFrame({c: pd.array(list(v), dtype="Int64" if c in ("t_send_ns", "order_id", "causal_parent")
                                         else ("string" if c == "msg_type" else "int64"))
                            for c, v in zip(MSG_COLS, cols)})
        ref = pd.read_parquet(refdir / "trace.parquet")
        refm = pd.read_parquet(refdir / "message_trace.parquet")
        c1, i1 = first_diff(tr, ref, TRACE_COLS)
        c2, i2 = first_diff(led, refm, MSG_COLS)
        status = "OK" if c1 is None and c2 is None else "DIFF"
        print(f"{status:4s} {name:45s} rows={len(tr):8d}/{len(ref):8d} msgs={len(led):8d}/{len(refm):8d} "
              f"{dt:6.2f}s trace_diff={c1}@{i1 if c1 else ''} ledger_diff={c2}@{i2 if c2 else ''}", flush=True)
        if status == "OK":
            ok += 1
        else:
            bad.append(name)
    print(f"\n{ok} exact, {len(bad)} differ: {bad}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "*")
