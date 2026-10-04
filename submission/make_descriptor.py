"""Build submission.json for Track 3 from the toolkit's published fixture.

usage: python submission/make_descriptor.py <registry> <repository> <sha256:digest> [dev|final] [out]
e.g.   python submission/make_descriptor.py docker.io tps3108/agenthon-t3 sha256:abc... dev

Starts from qfbench2_common/contracts/fixtures/c5/simulation_dev.json (the guide says never to
hand-write a descriptor), sets the image object, reseals descriptor_digest with the toolkit and
validates with SubmissionDescriptor.from_mapping. team_id stays the fixture placeholder:
`qfbench2 submission pack` derives the real one from your team number + Team Key and reseals.
"""
import json
import pathlib
import re
import sys

import qfbench2_common
from qfbench2_common.contracts.descriptor import SubmissionDescriptor, seal_descriptor_digest


def main():
    registry, repository, digest = sys.argv[1:4]
    phase = sys.argv[4] if len(sys.argv) > 4 else "dev"
    out = pathlib.Path(sys.argv[5] if len(sys.argv) > 5 else "submission/submission.json")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
        raise SystemExit(f"digest must look like sha256:<64 hex>, got {digest!r}")
    fx = pathlib.Path(qfbench2_common.__file__).parent / "contracts/fixtures/c5/simulation_dev.json"
    d = json.loads(fx.read_text())
    d["image"] = {"registry": registry, "repository": repository, "digest": digest}
    d["phase"] = phase
    d["competition_id"] = f"agenthon2026-simulation-{phase}"
    d["models"] = []
    d["license"] = "Apache-2.0"
    d.pop("descriptor_digest", None)
    sealed = seal_descriptor_digest(d)
    SubmissionDescriptor.from_mapping(sealed)  # raises on any contract violation
    # `qfbench2 submission pack` derives team_id from the team number + key and reseals; it refuses
    # a descriptor whose team_id disagrees, so hand it the validated body without one.
    sealed = {k: v for k, v in sealed.items() if k not in ("team_id", "descriptor_digest")}
    out.write_text(json.dumps(sealed, indent=2) + "\n")
    print(f"wrote {out}\n{json.dumps(sealed, indent=2)}")


if __name__ == "__main__":
    main()
