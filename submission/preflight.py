"""Pre-upload gate for a Track-3 candidate image. Every check must pass before submitting.

usage: python submission/preflight.py <image> [--quick]

1. image contract: linux/amd64, LABEL qfbench2.interface_version="2.0", verbs on PATH
2. outputs: /output holds only deliverables; outputs byte-identical across repeats and with --gpus
3. official public regression (65 units, run_regression.py)
4. batch units (6) with the official aggregate + isolation gates
5. adversarial shadow-sealed suite vs the real ABIDES baseline (skipped with --quick)
"""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PUB = ROOT / "upstream/track3-simulation-public"
PY = sys.executable
ok_all = True


def check(name, cond, detail=""):
    global ok_all
    ok_all &= bool(cond)
    print(f"[{'PASS' if cond else 'FAIL'}] {name} {detail}", flush=True)


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def run_unit(img, unit, extra=()):
    td = pathlib.Path(tempfile.mkdtemp())
    u = PUB / "units" / unit
    r = sh(["docker", "run", "--rm", "--network", "none", "--cpus", "4", "--memory", "16g", *extra,
            "-v", f"{u}:/input:ro", "-v", f"{td}:/output", img,
            "simulate", "--config", "/input/scenario.json", "--out", "/output/trace.parquet"])
    return r, td


def main():
    img = sys.argv[1]
    quick = "--quick" in sys.argv
    insp = json.loads(sh(["docker", "image", "inspect", img]).stdout)[0]
    check("platform linux/amd64", insp["Os"] == "linux" and insp["Architecture"] == "amd64")
    check("interface label 2.0", (insp["Config"].get("Labels") or {}).get("qfbench2.interface_version") == "2.0")
    for verb in ("simulate", "simulate-batch"):
        r = sh(["docker", "run", "--rm", "--network", "none", img, verb, "--help"])
        check(f"verb '{verb}' resolves on PATH", r.returncode == 0)

    r1, d1 = run_unit(img, "t3-mp05-cancel-churn-newest")
    check("simulate exits 0 (unit dir mounted at /input)", r1.returncode == 0, r1.stderr[-200:])
    files = sorted(p.name for p in d1.iterdir())
    check("output holds only deliverables", files == ["events.json", "message_trace.parquet", "trace.parquet"], str(files))
    ev = json.loads((d1 / "events.json").read_text())
    need = {"scenario_id", "seed", "n_events", "wall_clock_sec", "events_per_sec", "trace_sha256", "peak_memory_bytes", "gpu_seconds"}
    check("events.json fields", need <= set(ev), str(sorted(need - set(ev))))
    check("trace_sha256 matches file", ev["trace_sha256"] == hashlib.sha256((d1 / "trace.parquet").read_bytes()).hexdigest())
    eps = ev["n_events"] / ev["wall_clock_sec"]
    check("events_per_sec consistent (±5%)", abs(ev["events_per_sec"] - eps) / eps < 0.05)
    digest = lambda d: [hashlib.sha256((d / f).read_bytes()).hexdigest() for f in ("trace.parquet", "message_trace.parquet")]
    _, d2 = run_unit(img, "t3-mp05-cancel-churn-newest")
    check("byte-identical repeat", digest(d1) == digest(d2))
    r3, d3 = run_unit(img, "t3-mp05-cancel-churn-newest", ["--gpus", "all"])
    check("runs with --gpus all, identical output", r3.returncode == 0 and digest(d3) == digest(d1), r3.stderr[-150:])

    env = dict(os.environ, PYTHONPATH=str(PUB))
    r = sh([PY, "regression_suite/run_regression.py", "--candidate-image", img, "--scenarios-dir", "regression_suite/scenarios/",
            "--reference-dir", "regression_suite/reference_traces/", "--output-dir", str(ROOT / "run_outputs/preflight"),
            "--workers", "4"], cwd=PUB, env=env)
    tail = [l for l in r.stdout.splitlines() if l.startswith("Total:")]
    check("official regression 65/65", tail and "Passed: 65" in tail[-1] and "Failed: 0" in tail[-1], tail[-1] if tail else r.stderr[-300:])
    r = sh([PY, str(ROOT / "engine/test/score_batches.py"), img], env=env)
    check("batch units 6/6 (aggregate + isolation)", "ALL PASS" in r.stdout, r.stdout.strip().splitlines()[-1] if r.stdout else r.stderr[-300:])
    if not quick:
        r = sh([PY, str(ROOT / "adv/adversarial.py"), "check", img])
        check("adversarial suite exact vs ABIDES", "ALL EXACT" in r.stdout, r.stdout.strip().splitlines()[-1] if r.stdout else r.stderr[-300:])
    print("\nPREFLIGHT", "PASSED" if ok_all else "FAILED")
    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()
