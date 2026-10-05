# Agenthon 2026 Track 3 — `t3sim`: an exact, compiled ABIDES-equivalent market simulator

`t3sim` is a single static C++17 binary that reproduces the pinned ABIDES baseline
(`f9cbe51` + the four Track-3 patches + the `abides_fork` adapter) **row-for-row and
message-for-message**, and is shipped in a Docker image that implements the `simulate` and
`simulate-batch` verbs.

## Results (release candidate `t3sim:rc4`)

| Check | Result |
|---|---|
| Public single units vs shipped references (trace **and** ledger, exact) | **65 / 65** |
| Public batch sub-scenarios vs shipped references (exact) | **30 / 30** |
| Official `run_regression.py` | **65 / 65 PASS** |
| Official batch gates (`check_aggregate` + `score_isolation`) | **6 / 6 PASS** |
| Adversarial shadow-sealed suite vs the real ABIDES image (exact) | **36 / 36** |
| Byte-identical repeats, also under `--gpus all` | yes |
| Mean container-window rate over all 71 public units (local Docker Desktop) | **~328k events/s** |
| Same metric, mega unit (1.17M rows) | ~1.56M events/s (ABIDES: ~10.5k) |
| Scale test, 13.9M rows, under Final caps (4 CPU, 16 GiB) | 6.8 s container, ~2.0M events/s, 477 MiB peak |

ABIDES's own shipped `events.json` files self-report ~14k events/s, timing the simulation loop
only. The figures above time the whole container, the way Final timing is measured.

Because the trace is exact, every Tier-B statistic and stylized-fact metric is 0, the same as the
baseline compared with itself.

## How it works

1. **The spec model.** `spec/spec_sim.py` restates, in about 500 lines, exactly the ABIDES code
   paths the scenario agents reach. It matches all 95 public cases exactly.
2. **The C++ engine** (`engine/src/`) implements those semantics again with:
   - a bit-exact port of numpy's legacy `RandomState` (`rng.hpp`). MT19937, polar gauss,
     masked `randint` and legacy pareto are verified against numpy draw by draw.
   - the ABIDES kernel order `(time, sender, recipient, message_id)`, using split 4-ary heaps for
     wakeups and messages, plus per-agent busy times and computation delays.
   - Python numeric semantics where they matter: the oracle mixes int and float times after a
     megashock, compares floats against ints exactly, and rounds half-to-even.
   - the exchange, order book, STP, protocol delays and the 16:00 oracle close.
3. **Speed.**
   - Output streams in 4,096-row chunks to encoder threads, which write Parquet pages
     (delta-binary-packed integers, dictionary strings) while the simulation runs.
   - A producer thread precomputes the latency RNG stream. Its values depend only on the seed,
     so precomputing them is exact.
   - The binary is static, starts in about a millisecond, and computes SHA-256 with SHA-NI.
4. **Exactness rules.** Keep `-ffp-contract=off`, do not use `-ffast-math`, and link against
   glibc libm. Windows' UCRT `log` differs from glibc in the last ULP.

## Reproduce

```bash
# scoring env (py3.13 + toolkit v2.5.1) at .venv-score; ABIDES baseline image from an LF copy of baselines/
docker build --platform=linux/amd64 -f submission/Dockerfile -t t3sim:dev .   # context = repo root
python submission/preflight.py t3sim:dev            # all gates; --quick skips the adversarial suite
python engine/test/container_timing.py t3sim:dev 3  # organizer-style container-window rates
python adv/adversarial.py gen|ref|check IMAGE       # shadow-sealed suite vs ABIDES
```

## Submitting (needs your accounts)

1. `docker tag t3sim:rc4 <dockerhub-user>/agenthon-t3:rc4 && docker push <dockerhub-user>/agenthon-t3:rc4`.
   The repository must allow **anonymous pulls by digest**.
2. `docker inspect --format '{{index .RepoDigests 0}}' <dockerhub-user>/agenthon-t3:rc4` gives the digest.
3. `python submission/make_descriptor.py docker.io <dockerhub-user>/agenthon-t3 sha256:<digest> dev`
   builds a sealed, validated `submission/submission.json`.
4. `qfbench2 submission pack --descriptor submission/submission.json --team-number <N> --out submission.zip`
   prompts for the Team Key. Then upload `submission.zip` on the Track 3 CodaBench page
   (limits: 5 per day, 20 in total).

## Risk register

| Risk | Mitigation |
|---|---|
| A sealed scenario uses a code path no public unit covers | The adversarial suite covers protocol delays, both STP policies, a missing latency config, sessions past the oracle close, rejected prices, float parameters, extreme jumps, 380 agents and sub-round-trip wakeups. All exact. |
| libm or compiler FP differences | glibc static link, FP contraction off. Validated in the Docker image itself. |
| Memory or time on a much larger SS-BENCH | Streaming output keeps RSS small (54 MB for 1.17M rows). Throughput is about 5M rows/s per core. |
| Message ids ≥ 2³², or more than 2³¹ rows in a page | Not reachable at any size ABIDES can generate. |

## Release rule (learned from rc3)

rc3 passed every correctness gate yet was 4% slower on mid-size units: its write-thread threshold
was tuned on a subset that missed the mid-size units. Any new release candidate must therefore pass
**both** `submission/preflight.py` **and** a head-to-head race of the *shipped image binary* against
the previous RC on all 71 units (`engine/test/rc_ab_all.sh`, then `engine/test/recheck.sh` with 9
repeats for any unit that looks slower). Development scores: rc2 8,703,131.73; rc3 8,584,091.99 (regression);
rc4 = fix, ≥ rc2 on every unit.
