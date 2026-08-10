#!/usr/bin/env python3
"""Regenerate the golden .pb fixtures from their .json sources.

The .pb files are produced by **Google's** protobuf implementation, and read
back by **nanopb** in the C tests. That asymmetry is the entire point: it is
what turns the round-trip suite into a cross-implementation check rather than
nanopb agreeing with itself. protobuf.js in the browser is the third
implementation and shares Google's canonical semantics, so agreement here is
strong evidence the schema is unambiguous.

The fixtures are committed. That is deliberate — they pin the wire format, so
renumbering a field or changing a type breaks the tests loudly instead of
silently orphaning every document written by an older build (the compatibility
rules in proto/pinled.proto).

Regenerating is therefore a decision, not a chore. If a fixture changes and you
did not intend to change the wire format, that is the bug.

    protoc -I proto --python_out=<tmp> proto/pinled.proto
    python3 test/host/fixtures/regenerate.py --bindings <tmp>

Requires `pip install protobuf`. Nothing else in test/host needs it.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent

# fixture stem -> message type
FIXTURES = {
    "machine_profile": "MachineProfile",
    "install_config": "InstallConfig",
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--bindings",
        required=True,
        help="directory containing pinled_pb2.py (protoc --python_out)",
    )
    ap.add_argument(
        "--check",
        action="store_true",
        help="verify committed fixtures match, without rewriting them",
    )
    args = ap.parse_args()

    sys.path.insert(0, args.bindings)
    try:
        import pinled_pb2 as pb
        from google.protobuf import json_format
    except ImportError as exc:
        print(f"regenerate: {exc}", file=sys.stderr)
        print("  protoc -I proto --python_out=<dir> proto/pinled.proto", file=sys.stderr)
        print("  pip install protobuf", file=sys.stderr)
        return 2

    drift = 0
    for stem, type_name in FIXTURES.items():
        src = HERE / f"{stem}.json"
        dst = HERE / f"{stem}.pb"

        msg = getattr(pb, type_name)()
        json_format.Parse(src.read_text(encoding="utf-8"), msg)

        # deterministic=True keeps map ordering stable. There are no maps in
        # this schema today, but a future one would otherwise make the
        # fixtures spuriously unstable and train everyone to ignore drift.
        encoded = msg.SerializeToString(deterministic=True)

        if args.check:
            existing = dst.read_bytes() if dst.exists() else b""
            status = "ok" if existing == encoded else "DRIFT"
            if existing != encoded:
                drift += 1
            print(f"{status:5} {dst.name}  {len(encoded)} bytes")
        else:
            dst.write_bytes(encoded)
            print(f"wrote {dst.name}  {len(encoded)} bytes  {encoded.hex()}")

    if args.check and drift:
        print(f"\n{drift} fixture(s) differ from their .json source", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
