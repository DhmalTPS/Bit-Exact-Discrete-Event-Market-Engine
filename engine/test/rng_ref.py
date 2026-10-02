import numpy as np
for s in [0, 1, 42, 1001, 2147483647, 4294967295]:
    g = np.random.RandomState(seed=np.uint64(s))
    for _ in range(3): print(f"u32 {s} {g.randint(low=0, high=2**32, dtype='uint64')}")
    for _ in range(5): print(f"nrm {s} {float(g.normal(10.0, 2.0))!r}")
    for _ in range(5): print(f"lgn {s} {float(g.lognormal(mean=float(np.log(500.0)), sigma=0.5))!r}")
    for _ in range(5): print(f"par {s} {float(g.pareto(1.5))!r}")
    for _ in range(5): print(f"uni {s} {float(g.uniform(100.0, 2000.0))!r}")
    for _ in range(5): print(f"exp {s} {float(g.exponential(scale=5e8))!r}")
    for _ in range(8): print(f"ri2 {s} {g.randint(0, 2)}")
    for _ in range(8): print(f"ri6 {s} {g.randint(0, 6)}")
    for _ in range(4): print(f"ri1 {s} {g.randint(0, 1)}")
    for _ in range(3): print(f"nrm {s} {float(g.normal(0.0, 0.0))!r}")
