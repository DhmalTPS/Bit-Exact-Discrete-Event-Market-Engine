# Local-only helper: runs the pinned qfbench2 toolkit on Linux so the Team Key file can be mode 0600.
FROM python:3.13-slim
RUN apt-get update && apt-get install -y --no-install-recommends git && rm -rf /var/lib/apt/lists/* \
 && pip install --no-cache-dir "qfbench2-common @ git+https://github.com/Agenthon-2026/Agenthon2026-public.git@v2.5.1#subdirectory=common"
WORKDIR /work
