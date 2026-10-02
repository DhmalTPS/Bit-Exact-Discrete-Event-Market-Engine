"""Measure the organizer's ranked quantity locally: rows / (State.FinishedAt - State.StartedAt) of
the container, per unit, median of N repeats (first repeat discarded as warm-up)."""
import datetime as dt
import json
import pathlib
import statistics
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).parents[2]
U = ROOT / "upstream/track3-simulation-public/units"


def ts(s):  # docker RFC3339 with ns precision
    s = s.rstrip("Z")
    base, frac = (s.split(".") + ["0"])[:2]
    return dt.datetime.fromisoformat(base).timestamp() + float("0." + frac)


def run(img, unit, gpus=False):
    u = U / unit
    with tempfile.TemporaryDirectory() as td:
        cmd = ["docker", "create", "--network", "none", "--cpus", "4", "--memory", "16g"]
        if gpus:
            cmd += ["--gpus", "all"]
        if (u / "scenario.json").exists():
            cmd += ["-v", f"{u / 'scenario.json'}:/input/scenario.json:ro", "-v", f"{td}:/output", img,
                    "simulate", "--config", "/input/scenario.json", "--out", "/output/trace.parquet"]
        else:
            cmd += ["-v", f"{u / 'scenarios'}:/input/scenarios:ro", "-v", f"{td}:/output", img,
                    "simulate-batch", "--batch-dir", "/input/scenarios", "--out-dir", "/output"]
        cid = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout.strip()
        subprocess.run(["docker", "start", "-a", cid], check=True, capture_output=True)
        st = json.loads(subprocess.run(["docker", "inspect", cid], check=True, capture_output=True, text=True).stdout)[0]["State"]
        subprocess.run(["docker", "rm", cid], capture_output=True)
        el = ts(st["FinishedAt"]) - ts(st["StartedAt"])
        p = pathlib.Path(td)
        if (p / "events.json").exists():
            n = json.loads((p / "events.json").read_text())["n_events"]
        else:
            n = json.loads((p / "batch_events.json").read_text())["total_events"]
        return n, el


def main():
    img = sys.argv[1]
    reps = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    units = sys.argv[3:] or ["t3-s001-price-time-priority", "t3-eq-uniform-tight", "t3-fastlob-core",
                             "t3-mp05-cancel-churn-newest", "t3-gb-mega-throughput", "t3-gbatch-homog-8"]
    for unit in units:
        res = [run(img, unit) for _ in range(reps)]
        n = res[0][0]
        els = [e for _, e in res[1:]] if reps > 1 else [res[0][1]]
        med = statistics.median(els)
        print(f"{unit:32s} rows={n:9d} container_s={med:8.4f} rate={n / med:12.0f} ev/s  (all: {[round(e, 4) for _, e in res]})", flush=True)


if __name__ == "__main__":
    main()
