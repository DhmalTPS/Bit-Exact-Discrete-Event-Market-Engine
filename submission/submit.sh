#!/bin/bash
# One-command Track-3 submission. Run from Git Bash at the repo root:
#     bash submission/submit.sh rc2 dev          # Development
#     bash submission/submit.sh rcN final        # Final (Oct 13-25)
# Steps: preflight-tagged image -> push to Docker Hub -> verify anonymous pull -> sealed
# submission.json -> `qfbench2 submission pack` (Linux container, key file mode 0600) -> zip.
# The Team Key is read only by the toolkit from extra_info/team_key.txt; it is never printed.
set -euo pipefail
TAG=${1:-rc2}
PHASE=${2:-dev}
REPO=${T3_REPO:-tps3108/agenthon-t3}
cd "$(dirname "$0")/.."
export MSYS_NO_PATHCONV=1
ROOT_W=$(pwd -W 2>/dev/null || pwd)

echo "== 1/5 tag + push ${REPO}:${TAG}"
docker tag "t3sim:${TAG}" "${REPO}:${TAG}"
docker push "${REPO}:${TAG}"
DIGEST=$(docker inspect --format '{{index .RepoDigests 0}}' "${REPO}:${TAG}" | sed 's/.*@//')
echo "digest: ${DIGEST}"

echo "== 2/5 anonymous pull check (the organizers pull without credentials)"
TOKEN=$(curl -fsS "https://auth.docker.io/token?service=registry.docker.io&scope=repository:${REPO}:pull" \
        | python -c "import json,sys; print(json.load(sys.stdin)['token'])")
CODE=$(curl -s -o /dev/null -w '%{http_code}' -H "Authorization: Bearer ${TOKEN}" \
       -H "Accept: application/vnd.docker.distribution.manifest.v2+json, application/vnd.oci.image.index.v1+json, application/vnd.oci.image.manifest.v1+json, application/vnd.docker.distribution.manifest.list.v2+json" \
       "https://registry-1.docker.io/v2/${REPO}/manifests/${DIGEST}")
if [ "$CODE" != "200" ]; then
    echo "ANONYMOUS PULL FAILED (HTTP ${CODE}): make ${REPO} PUBLIC on hub.docker.com, then re-run." >&2
    exit 1
fi
echo "anonymous pull by digest: OK"

echo "== 3/5 descriptor"
.venv-score/Scripts/python submission/make_descriptor.py docker.io "${REPO}" "${DIGEST}" "${PHASE}" submission/submission.json > /dev/null
echo "submission/submission.json written + validated (${PHASE})"

echo "== 4/5 pack (toolkit in Linux container; key file chmod 600 inside the container)"
OUT="submission-${PHASE}-${TAG}.zip"
docker run --rm -v "${ROOT_W}/extra_info:/secrets:ro" -v "${ROOT_W}/submission:/work/submission" t3-packer:local sh -c \
  'cp /secrets/team_key.txt /tmp/k && chmod 600 /tmp/k && \
   qfbench2 submission pack --team-number "$(tr -d "[:space:]" < /secrets/team_number.txt)" --team-key-file /tmp/k \
     --descriptor /work/submission/submission.json --out "/work/submission/'"${OUT}"'" --force > /dev/null && rm -f /tmp/k'

echo "== 5/5 done"
echo "Upload submission/${OUT} on the Track 3 CodaBench page (team account)."
