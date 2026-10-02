# Track 3 submission: t3sim, an exact compiled re-implementation of the ABIDES baseline.
# Build context: the repository root (needs engine/src).
FROM --platform=linux/amd64 debian:bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
COPY engine/src /src
# -ffp-contract=off: never fuse a*b+c (CPython/numpy round each op separately).
# No -ffast-math: every libm call must be the real glibc routine numpy/CPython use.
RUN g++ -O2 -std=c++17 -march=x86-64-v2 -mtune=sapphirerapids -ffp-contract=off \
        -fno-plt -static -o /t3sim /src/main.cpp -lpthread \
    && strip /t3sim && /t3sim --help

FROM --platform=linux/amd64 debian:bookworm-slim
LABEL qfbench2.interface_version="2.0"
COPY --from=build /t3sim /usr/local/bin/simulate
RUN ln /usr/local/bin/simulate /usr/local/bin/simulate-batch
WORKDIR /work
CMD ["simulate", "--help"]
