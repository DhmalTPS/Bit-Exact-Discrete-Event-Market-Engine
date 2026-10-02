"""Adversarial shadow-sealed suite.

Generates scenarios along the axes the sealed families are documented to stress (and along edge
cases of the baseline code itself), produces the ground truth with the real ABIDES baseline image,
runs the candidate image, and compares trace + ledger exactly. Anything not exact is a bug.

usage: python adv/adversarial.py gen            # write adv/scenarios/*.json
       python adv/adversarial.py ref [N]        # run baseline image -> adv/ref/<name>/
       python adv/adversarial.py check IMAGE    # run IMAGE -> adv/cand/<name>/, compare
"""
import concurrent.futures as cf
import copy
import json
import pathlib
import random
import subprocess
import sys
import uuid

import pandas as pd

ROOT = pathlib.Path(__file__).parent
SC = ROOT / "scenarios"
BASELINE = "track3-abides-baseline:latest"


def base(seed, horizon_s=1.0):
    return {
        "scenario_id": str(uuid.UUID(int=random.Random(seed).getrandbits(128))),
        "scenario_family": "matching-engine-semantics",
        "schema_version": 2,
        "seed": seed,
        "horizon_ns": int(horizon_s * 1e9),
        "exchange_config": {"symbol": "SYM", "tick_size": 1, "lot_size": 1, "min_price": 1,
                            "max_price": 1000000, "stp_policy": "cancel_newest"},
        "oracle_config": {"type": "mean_reverting", "params": {
            "initial_price": 100000, "kappa": 0.05, "sigma": 0.001, "dt_ns": 1000000,
            "noise_type": "gaussian", "jump_intensity": 0.0, "jump_sigma": 0.0}},
        "latency_config": {"model": "log_normal", "params": {"mean_ns": 5000, "sigma": 0.5,
                                                              "min_ns": 500, "max_ns": 200000}},
        "agent_configs": [
            {"agent_type": "NoiseTrader", "count": 6, "params": {"order_size_mean": 10, "order_size_std": 3,
                                                                 "arrival_rate_hz": 200.0, "price_offset_ticks": 4}},
            {"agent_type": "ValueTrader", "count": 2, "params": {"order_size_mean": 20, "threshold_ticks": 2,
                                                                 "arrival_rate_hz": 100.0}},
            {"agent_type": "MomentumTrader", "count": 2, "params": {"order_size_mean": 15, "threshold_ticks": 1,
                                                                    "lookback": 4, "arrival_rate_hz": 100.0}},
            {"agent_type": "MarketMaker", "count": 1, "params": {"spread_ticks": 2, "depth_levels": 3,
                                                                 "size_per_level": 20,
                                                                 "rebalance_interval_ns": 20000000}},
        ],
    }


def agent(sc, typ):
    return next(a for a in sc["agent_configs"] if a["agent_type"] == typ)


def scenarios():
    out = {}
    s = 9000
    # 1. exchange protocol: ack/pipeline + compute delays, both STP policies
    for i, (ack, comp, stp) in enumerate([(0, 0, "cancel_newest"), (500, 0, "cancel_oldest"), (5000, 100, "cancel_newest"),
                                          (50000, 2000, "cancel_oldest"), (0, 3000, "cancel_newest"), (200, 50, "weird_policy")]):
        sc = base(s + i)
        sc["scenario_family"] = "exchange-protocol"
        sc["exchange_config"].update(protocol_enforcement=True, stp_policy=stp, ack_delay_ns=ack, compute_delay_ns=comp)
        agent(sc, "MarketMaker")["count"] = 2
        agent(sc, "MarketMaker")["params"]["rebalance_interval_ns"] = 3000000
        out[f"proto_ack{ack}_comp{comp}_{stp}"] = sc
    # 2. no latency_config (ABIDES default line-distance model)
    sc = base(s + 20); del sc["latency_config"]; out["no_latency_config"] = sc
    # 3. latency models / ties
    sc = base(s + 21); sc["latency_config"] = {"model": "deterministic", "params": {"mean_ns": 1000, "min_ns": 0, "max_ns": 1e9}}
    for a in sc["agent_configs"]:
        a["params"].pop("arrival_rate_hz", None); a["params"]["rebalance_interval_ns"] = 5000000
    out["deterministic_ties_same_interval"] = sc
    sc = base(s + 22); sc["latency_config"] = {"model": "pareto", "params": {"alpha": 0.7, "min_ns": 300, "max_ns": 5e7}}
    out["pareto_alpha07_extreme_tail"] = sc
    sc = base(s + 23); sc["latency_config"] = {"model": "uniform", "params": {"mean_ns": 1, "min_ns": 0, "max_ns": 3}}
    out["uniform_tiny_dense_ties"] = sc
    sc = base(s + 24); sc["latency_config"]["params"].update(sigma=2.5, mean_ns=20000, max_ns=1e7)
    out["lognormal_sigma25"] = sc
    sc = base(s + 25); sc["latency_config"] = {"model": "uniform", "params": {"min_ns": 5000, "max_ns": 100}}
    out["uniform_min_gt_max"] = sc
    # 4. sessions crossing the 16:00 oracle close (observe() clamps to oracle close - 1)
    sc = base(s + 30, horizon_s=7 * 3600)
    for a in sc["agent_configs"]:
        a["params"].pop("arrival_rate_hz", None); a["params"].pop("rebalance_interval_ns", None)
        a["params"]["arrival_rate_hz"] = 0.01
    agent(sc, "MarketMaker")["params"]["rebalance_interval_ns"] = 120 * 10**9
    agent(sc, "MarketMaker")["params"].pop("arrival_rate_hz", None)
    sc["oracle_config"]["params"].update(jump_intensity=0.002, jump_sigma=500.0, kappa=0.0005)
    out["long_session_past_oracle_close"] = sc
    # 5. extreme mixes
    sc = base(s + 40); sc["agent_configs"] = [agent(sc, "MarketMaker")]; agent(sc, "MarketMaker")["count"] = 3
    out["mm_only_three"] = sc
    sc = base(s + 41); sc["agent_configs"] = [agent(sc, "NoiseTrader")]; agent(sc, "NoiseTrader")["count"] = 12
    out["noise_only_no_mm"] = sc
    sc = base(s + 42); sc["agent_configs"] = [agent(sc, "ValueTrader"), agent(sc, "MomentumTrader")]
    out["value_momentum_empty_book"] = sc
    sc = base(s + 43); agent(sc, "MomentumTrader")["count"] = 12; agent(sc, "MomentumTrader")["params"]["threshold_ticks"] = 0
    out["momentum_dominated"] = sc
    sc = base(s + 44); agent(sc, "ValueTrader")["count"] = 12; agent(sc, "ValueTrader")["params"]["threshold_ticks"] = 0
    sc["oracle_config"]["params"].update(kappa=2.0, sigma=0.01)
    out["value_dominated_fast_reversion"] = sc
    # 6. tiny prices -> negative MM/noise prices are rejected by the book
    sc = base(s + 50); sc["oracle_config"]["params"]["initial_price"] = 3
    agent(sc, "MarketMaker")["params"].update(depth_levels=6, spread_ticks=5)
    out["tiny_price_negative_orders"] = sc
    # 7. odd params, float-typed params
    sc = base(s + 60)
    agent(sc, "NoiseTrader")["params"].update(price_offset_ticks=0, order_size_mean=10.5, order_size_std=0.0)
    agent(sc, "MarketMaker")["params"].update(spread_ticks=3.7, depth_levels=1.9, size_per_level=7.5)
    agent(sc, "MomentumTrader")["params"].update(lookback=2.7, threshold_ticks=1.5, order_size_mean=2.5)
    agent(sc, "ValueTrader")["params"].update(order_size_mean=0.4, arrival_rate_hz=3.0)
    agent(sc, "NoiseTrader")["count"] = 4.0
    out["float_typed_params"] = sc
    sc = base(s + 61); agent(sc, "MarketMaker")["params"].update(spread_ticks=1, depth_levels=10, size_per_level=1)
    out["mm_spread1_depth10_size1"] = sc
    # 8. wake interval shorter than round trip (overlapping queries; cancels of in-flight orders)
    sc = base(s + 70); sc["latency_config"]["params"].update(mean_ns=400000, min_ns=100000, max_ns=2000000)
    agent(sc, "NoiseTrader")["params"]["arrival_rate_hz"] = 5000.0
    agent(sc, "MarketMaker")["params"]["rebalance_interval_ns"] = 300000
    out["interval_shorter_than_roundtrip"] = sc
    # 9. oracle extremes
    sc = base(s + 80); sc["oracle_config"]["params"].update(jump_intensity=200.0, jump_sigma=0.0, kappa=0.0)
    out["oracle_frequent_megashocks_default_kappa"] = sc
    sc = base(s + 81); sc["oracle_config"]["params"]["scheduled_jump"] = {"time_ns": 0, "magnitude": -250000}
    agent(sc, "ValueTrader")["count"] = 5
    out["scheduled_jump_at_open_clamped_zero"] = sc
    sc = base(s + 82); sc["oracle_config"]["params"]["scheduled_jump"] = {"time_ns": 400000000.0, "magnitude": 3000}
    agent(sc, "ValueTrader")["count"] = 5
    out["scheduled_jump_float_time"] = sc
    sc = base(s + 83); sc["oracle_config"]["params"]["scheduled_jump"] = {"time_ns": 10**12, "magnitude": 500}
    out["scheduled_jump_after_close"] = sc
    # 10. population / scale
    sc = base(s + 90, horizon_s=0.25); agent(sc, "NoiseTrader")["count"] = 300; agent(sc, "MarketMaker")["count"] = 8
    agent(sc, "ValueTrader")["count"] = 40; agent(sc, "MomentumTrader")["count"] = 30
    out["population_380"] = sc
    sc = base(s + 91, horizon_s=0.4); agent(sc, "NoiseTrader")["params"]["arrival_rate_hz"] = 20000.0
    agent(sc, "NoiseTrader")["params"]["price_offset_ticks"] = 40
    out["deep_book_high_rate"] = sc
    # 11. random fuzz around the public envelope
    rng = random.Random(1234)
    for i in range(10):
        sc = base(10000 + i, horizon_s=rng.choice([0.3, 0.6, 1.0]))
        sc["latency_config"] = rng.choice([
            {"model": "log_normal", "params": {"mean_ns": rng.choice([300, 2000, 20000]), "sigma": rng.choice([0.2, 0.8, 1.5]), "min_ns": 50, "max_ns": 1e6}},
            {"model": "uniform", "params": {"min_ns": 10, "max_ns": rng.choice([100, 5000, 100000])}},
            {"model": "pareto", "params": {"alpha": rng.choice([1.1, 1.5, 3.0]), "min_ns": rng.choice([0, 100, 1000]), "max_ns": 1e7}},
            {"model": "deterministic", "params": {"mean_ns": rng.choice([0, 7, 1000])}},
        ])
        for a in sc["agent_configs"]:
            a["count"] = rng.choice([0, 1, 2, 5, 9])
        agent(sc, "MarketMaker")["count"] = rng.choice([0, 1, 2])
        sc["oracle_config"]["params"].update(kappa=rng.choice([0.01, 0.1, 1.0]), sigma=rng.choice([0.0005, 0.005]),
                                             jump_intensity=rng.choice([0.0, 2.0, 20.0]), jump_sigma=rng.choice([0.0, 300.0]))
        if rng.random() < 0.5:
            sc["exchange_config"].update(protocol_enforcement=True, stp_policy=rng.choice(["cancel_newest", "cancel_oldest"]),
                                         ack_delay_ns=rng.choice([0, 1000]), compute_delay_ns=rng.choice([0, 500]))
        sc["seed"] = rng.randrange(2**31)
        out[f"fuzz_{i:02d}"] = sc
    return out


def docker_run(img, scen, outdir, timeout=900):
    outdir.mkdir(parents=True, exist_ok=True)
    subprocess.run(["docker", "run", "--rm", "--network", "none", "-v", f"{scen}:/input/scenario.json:ro",
                    "-v", f"{outdir}:/output", img, "simulate", "--config", "/input/scenario.json",
                    "--out", "/output/trace.parquet"], check=True, capture_output=True, timeout=timeout)


def same(a: pd.DataFrame, b: pd.DataFrame) -> str:
    if len(a) != len(b):
        return f"rows {len(a)} vs {len(b)}"
    for c in b.columns:
        x, y = a[c].astype("object").to_numpy(), b[c].astype("object").to_numpy()
        for i in range(len(x)):
            if not ((pd.isna(x[i]) and pd.isna(y[i])) or (not pd.isna(x[i]) and not pd.isna(y[i]) and x[i] == y[i])):
                return f"{c}@{i}: {x[i]} vs {y[i]}"
    return ""


def main():
    cmd = sys.argv[1]
    if cmd == "gen":
        SC.mkdir(parents=True, exist_ok=True)
        for name, sc in scenarios().items():
            (SC / f"{name}.json").write_text(json.dumps(sc, indent=1))
        print(len(list(SC.glob("*.json"))), "scenarios")
    elif cmd == "ref":
        workers = int(sys.argv[2]) if len(sys.argv) > 2 else 4
        todo = [p for p in sorted(SC.glob("*.json")) if not (ROOT / "ref" / p.stem / "events.json").exists()]

        def job(p):
            try:
                docker_run(BASELINE, p, ROOT / "ref" / p.stem, timeout=3600)
                return p.stem, json.loads((ROOT / "ref" / p.stem / "events.json").read_text())["n_events"]
            except subprocess.CalledProcessError as e:
                return p.stem, "BASELINE-ERROR " + e.stderr.decode()[-300:].replace("\n", " | ")
        with cf.ThreadPoolExecutor(workers) as ex:
            for name, r in ex.map(job, todo):
                print(f"{name:45s} {r}", flush=True)
    elif cmd == "check":
        img = sys.argv[2]
        bad = 0
        for p in sorted(SC.glob("*.json")):
            ref = ROOT / "ref" / p.stem
            if not (ref / "trace.parquet").exists():
                print(f"SKIP {p.stem} (no reference: baseline failed)")
                continue
            cand = ROOT / "cand" / p.stem
            try:
                docker_run(img, p, cand)
            except subprocess.CalledProcessError as e:
                print(f"FAIL {p.stem}: candidate error {e.stderr.decode()[-200:]}"); bad += 1; continue
            d1 = same(pd.read_parquet(cand / "trace.parquet"), pd.read_parquet(ref / "trace.parquet"))
            d2 = same(pd.read_parquet(cand / "message_trace.parquet"), pd.read_parquet(ref / "message_trace.parquet"))
            n = len(pd.read_parquet(ref / "trace.parquet"))
            print(f"{'OK  ' if not (d1 or d2) else 'FAIL'} {p.stem:45s} rows={n:7d} trace={d1 or 'exact'} ledger={d2 or 'exact'}", flush=True)
            bad += bool(d1 or d2)
        print("ALL EXACT" if not bad else f"{bad} FAILING")


if __name__ == "__main__":
    main()
