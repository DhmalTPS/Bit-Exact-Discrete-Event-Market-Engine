# Bit-Exact Discrete-Event Market Engine

`t3sim` is a single static C++17 binary that reproduces the pinned ABIDES market simulator
**row for row and message for message**, including a bit-exact port of NumPy's legacy MT19937
`RandomState`, at roughly 150x the baseline's throughput on the largest public unit.

## Context

- Agenthon 2026, Track 3 (simulation), run on CodaBench. Built 2 to 6 October 2026; solo.
- **Authorship:** the engine design, verification strategy and final submission are mine. I used AI coding assistants for parts of the implementation and for adversarial review.
- Final leaderboard rank: **23 / 55**.
- Organiser material (not included here): the Track 3 public kit
  [`Agenthon-2026/track3-simulation-public`](https://github.com/Agenthon-2026/track3-simulation-public),
  the programme kit [`Agenthon-2026/Agenthon2026-public`](https://github.com/Agenthon-2026/Agenthon2026-public),
  and the reference simulator [`jpmorganchase/abides-jpmc-public`](https://github.com/jpmorganchase/abides-jpmc-public)
  (pinned at `f9cbe51` plus the four Track 3 patches and the `abides_fork` adapter).

## Problem

Teams ship a Docker image with `simulate` and `simulate-batch` verbs. Given a scenario config
(agent populations, oracle, latency model, protocol delays, seed), the image must emit the same
event trace and ledger as the ABIDES baseline. Correctness is a hard gate: the fill sequence must be
exact, and sealed scenarios are used in the Final. Among correct entries, ranking is on organiser-measured
throughput (events/s over the whole container window) under Final caps of 4 CPUs and 16 GiB.

Exactness is the hard part. ABIDES is a Python discrete-event kernel driven by NumPy's legacy RNG, so
one extra or reordered random draw, one tie broken differently in the event queue, or one float rounded
differently by the platform's `libm` changes every subsequent row.

## Approach

1. **Spec model.** `spec/spec_sim.py` (about 560 lines of Python) restates exactly the ABIDES code paths
   the scenario agents reach. It matches all 95 public cases exactly and served as the oracle while building the engine.
2. **C++ engine** (`engine/src/`, about 2,800 lines):
   - a bit-exact port of NumPy's legacy `RandomState` (`rng.hpp`): MT19937, polar Gaussian, masked `randint`
     and legacy Pareto, checked draw by draw against NumPy (`engine/test/rng_test.cpp`, `rng_ref.py`);
   - the ABIDES kernel ordering `(time, sender, recipient, message_id)`, with separate 4-ary heaps for
     wakeups and messages, per-agent busy times and computation delays (`sim.hpp`);
   - Python numeric semantics where they matter: the oracle mixes int and float times after a megashock,
     compares floats against ints exactly, and rounds half-to-even;
   - the exchange, order book, self-trade prevention, protocol delays and the 16:00 oracle close.
3. **Throughput.**
   - Output streams in 4,096-row chunks to encoder threads that write Parquet pages (delta-binary-packed
     integers, dictionary strings) while the simulation runs (`output.hpp`, `parquet.hpp`).
   - A producer thread precomputes the latency RNG stream; its values depend only on the seed, so this is exact
     (`latstream.hpp`).
   - Small scenarios take a synchronous fast path; the binary is static, starts in about a millisecond, and
     hashes outputs with SHA-NI SHA-256 (`sha256.hpp`).
4. **Floating-point discipline.** Build with `-ffp-contract=off`, never `-ffast-math`, and link glibc `libm`
   statically: the Windows UCRT `log` differs from glibc in the last ULP, which is enough to break exactness.

## Results (release candidate `rc4`)

| Check | Result |
|---|---|
| Public single units vs shipped references (trace and ledger, exact) | **65 / 65** |
| Public batch sub-scenarios vs shipped references (exact) | **30 / 30** |
| Official `run_regression.py` | **65 / 65 PASS** |
| Official batch gates (`check_aggregate` + `score_isolation`) | **6 / 6 PASS** |
| Adversarial suite vs the real ABIDES image (exact) | **230 / 230** (36 targeted + 194 random fuzz); 8 further generated scenarios crash ABIDES itself and have no reference (`adv/check_rc4.txt`) |
| Byte-identical repeats, also under `--gpus all` | yes |
| Scale test, 13.9M rows, under Final caps (4 CPU, 16 GiB) | **6.8 s** container time, ~2.0M events/s, 477 MiB peak |
| Largest public unit (1.17M rows) | ~1.56M events/s vs ABIDES ~10.5k (**~150x**) |
| Mean container-window rate over all 71 public units (local Docker Desktop) | ~328k events/s |
| Final leaderboard | **23 / 55** |

ABIDES's own `events.json` files self-report ~14k events/s, timing the simulation loop only; the figures
above time the whole container, as the organiser does. Because the trace is exact, every distributional
(Tier-B) and stylized-fact metric is 0, the same as the baseline compared with itself.

**A regression that passed every correctness gate.** rc3 was exact everywhere yet ~4% slower on mid-size
units: its concurrent-ledger-write threshold had been tuned on a subset that missed them. Development scores
were rc2 8,703,131.73 and rc3 8,584,091.99. rc4 lowered the threshold from 1 MB to 64 KB and is at or above
rc2 on every unit (`analysis/rc2_vs_rc3.txt`). Since then a release candidate must pass both
`submission/preflight.py` and a head-to-head race of the shipped image binary against the previous release on
all 71 units (`engine/test/rc_ab_all.sh`, then `engine/test/recheck.sh` with 9 repeats for any unit that looks slower).

## Repository layout

```
engine/src/       C++17 engine: sim.hpp (kernel, agents, exchange), rng.hpp (NumPy RNG port),
                  latstream.hpp, output.hpp + parquet.hpp (streaming Parquet writer),
                  sha256.hpp, json.hpp, main.cpp (simulate / simulate-batch CLI)
engine/test/      RNG and SHA-256 unit tests, engine-vs-reference comparison, container timing,
                  A/B and release-candidate race scripts, profiling helpers
engine/tools/     one-off source patch helper
spec/             Python spec model of the ABIDES code paths and its comparison harness
adv/              adversarial suite: generator, 238 scenarios, logs; adv/scale/ holds the scale tests
analysis/         unit inventory, rc1 container rates, rc2/rc3 per-unit timing comparisons, fuzz logs
submission/       Dockerfile, preflight gate runner, descriptor builder, submit pipeline, packer image
```

Not included: the organiser kits and ABIDES (linked above), reference and candidate traces
(~600 MB), build outputs, environments, packed submissions and credentials.

## How to run

Requirements: Docker (linux/amd64), Python 3.13 with the organiser's `qfbench2` toolkit v2.5.1 for the
official gates, and the ABIDES baseline image built from the Track 3 kit for the adversarial suite.

```bash
# build the image (context = repo root)
docker build --platform=linux/amd64 -f submission/Dockerfile -t t3sim:dev .

# all correctness gates: public units, batches, official regression, batch gates, adversarial suite
python submission/preflight.py t3sim:dev            # --quick skips the adversarial suite

# organiser-style container-window throughput
python engine/test/container_timing.py t3sim:dev 3

# adversarial suite vs the real ABIDES image
python adv/adversarial.py gen                        # write adv/scenarios/*.json
python adv/adversarial.py ref                        # run the ABIDES baseline image -> adv/ref/
python adv/adversarial.py check t3sim:dev            # run the candidate -> adv/cand/, compare exactly
```

A native build needs only `g++` (C++17); see the flags in `submission/Dockerfile`.

## Notes and limitations

- The engine reproduces the ABIDES code paths that the Track 3 scenario agents reach; it is not a general
  ABIDES replacement.
- The 65/65, 30/30, 6/6 and scale-test figures come from `preflight.py` runs whose raw stdout was not saved;
  they are reproducible with the commands above. The adversarial result is saved in `adv/check_rc4.txt`.
- Throughput depends on the host: the per-unit rates were measured on local Docker Desktop, the scale
  test under `--cpus 4 --memory 16g`.
- "Rows" and "events" are the same quantity here: the organiser's rate is output trace rows (`n_events`)
  per second of container time.
- Limits not reachable at any size ABIDES can generate: message ids at or above 2^32, or more than 2^31 rows
  in one Parquet page.
