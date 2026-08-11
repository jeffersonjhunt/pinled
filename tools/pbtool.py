#!/usr/bin/env python3
"""Talk to a pinled device, and convert between protobuf and JSON.

This exists because choosing protobuf on the wire costs us `curl` (docs/WEBUI.md
§2a), and M3 is specifically the milestone meant to be exercisable before any UI
exists. So this is the replacement: same shape as curl, JSON in and out, binary
on the wire.

    tools/pbtool.py get  http://pinled.local/api/v1/config
    tools/pbtool.py put  http://pinled.local/api/v1/config my-config.json
    tools/pbtool.py info http://pinled.local

    tools/pbtool.py encode InstallConfig  < cfg.json > cfg.pb
    tools/pbtool.py decode InstallConfig  < cfg.pb   > cfg.json

The JSON is proto3's canonical JSON mapping, so what comes out of `decode` is
exactly what a shared profile looks like on disk and in the registry — not a
convenience format invented by this script.

Requirements (neither is needed to build or run the host tests):
    protoc                    to generate the Python bindings
    pip install protobuf      the runtime

Generate the bindings once:
    protoc -I proto --python_out=tools/_gen proto/pinled.proto
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import urllib.error
import urllib.request
import zlib

_GEN_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_gen")

# Endpoint -> message type. Saves spelling the type out for the common calls,
# and keeps one place to update when the API grows.
ENDPOINT_TYPES = {
    "info": "DeviceInfo",
    "config": "InstallConfig",
    "profile": "MachineProfile",
    "apply": "ApplyResult",
}

CONTENT_TYPE = "application/x-protobuf"


def _die(msg: str, hint: str = "") -> "NoReturn":  # type: ignore[name-defined]
    print(f"pbtool: {msg}", file=sys.stderr)
    if hint:
        print(f"        {hint}", file=sys.stderr)
    raise SystemExit(2)


def _load_bindings():
    """Import the generated bindings, with an actionable error if absent."""
    sys.path.insert(0, _GEN_DIR)
    try:
        import pinled_pb2  # type: ignore
    except ImportError:
        _die(
            "generated bindings not found",
            "run: protoc -I proto --python_out=tools/_gen proto/pinled.proto",
        )
    try:
        from google.protobuf import json_format  # noqa: F401
    except ImportError:
        _die("the protobuf runtime is missing", "run: pip install protobuf")
    return pinled_pb2


def _message(type_name: str):
    pb = _load_bindings()
    msg = getattr(pb, type_name, None)
    if msg is None:
        known = ", ".join(sorted(n for n in dir(pb) if n[:1].isupper()))
        _die(f"unknown message type {type_name!r}", f"known types: {known}")
    return msg()


def _to_json(msg) -> str:
    from google.protobuf import json_format

    # Printing defaults keeps the output stable and diffable: without it a
    # field that happens to be zero simply vanishes, and two profiles that
    # differ only in a default look identical.
    #
    # protobuf 5.26 renamed this argument (including_default_value_fields ->
    # always_print_fields_with_no_presence) and 7.x removed the old spelling.
    # Try the new name first and fall back, rather than pinning an old runtime
    # — this script has to keep working on whatever the build host has.
    for kw in ("always_print_fields_with_no_presence", "including_default_value_fields"):
        try:
            return json_format.MessageToJson(
                msg, **{kw: True}, preserving_proto_field_name=True
            )
        except TypeError:
            continue
    return json_format.MessageToJson(msg, preserving_proto_field_name=True)


def _from_json(msg, text: str):
    from google.protobuf import json_format

    try:
        json_format.Parse(text, msg)
    except json_format.ParseError as exc:
        _die(f"JSON does not match the schema: {exc}")
    return msg


def _endpoint_type(url: str) -> str:
    tail = url.rstrip("/").rsplit("/", 1)[-1]
    if tail not in ENDPOINT_TYPES:
        _die(
            f"cannot infer a message type for {url!r}",
            f"known endpoints: {', '.join(sorted(ENDPOINT_TYPES))}",
        )
    return ENDPOINT_TYPES[tail]


def _http(url: str, data: bytes | None = None, method: str = "GET") -> bytes:
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Accept", CONTENT_TYPE)
    if data is not None:
        req.add_header("Content-Type", CONTENT_TYPE)
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.read()
    except urllib.error.HTTPError as exc:
        _die(f"{method} {url} -> HTTP {exc.code} {exc.reason}")
    except urllib.error.URLError as exc:
        _die(f"cannot reach {url}: {exc.reason}", "is the board on the network?")


def cmd_get(args) -> int:
    msg = _message(args.type or _endpoint_type(args.url))
    msg.ParseFromString(_http(args.url))
    print(_to_json(msg))
    return 0


def cmd_put(args) -> int:
    msg = _message(args.type or _endpoint_type(args.url))
    with open(args.file, "r", encoding="utf-8") as fh:
        _from_json(msg, fh.read())
    body = _http(args.url, data=msg.SerializeToString(), method="PUT")

    # A successful apply restarts the board (FR-CFG-16), so an empty or
    # truncated response here is the expected outcome, not a failure.
    if not body:
        print("accepted; the board is restarting (FR-CFG-16)")
        return 0
    result = _message("ApplyResult")
    result.ParseFromString(body)
    print(_to_json(result))
    return 0


def cmd_info(args) -> int:
    url = args.url.rstrip("/")
    if not url.endswith("/info"):
        url += "/api/v1/info"
    msg = _message("DeviceInfo")
    msg.ParseFromString(_http(url))
    print(_to_json(msg))
    return 0


# --- stored-document framing -------------------------------------------------
#
# The 16-byte header from components/pinled_schema/include/pinled_doc_frame.h,
# reimplemented rather than shared because there is nothing to share: it is
# four fields and a checksum. The value of a second implementation is the same
# as everywhere else in this project — nanopb agreeing with Google's protobuf,
# and now this agreeing with the C. If they disagree the format is ambiguous.
#
# zlib.crc32 is CRC-32/ISO-HDLC, the same one pinled_crc32.cpp computes.

_MAGIC = b"PLD1"
_FRAME_VERSION = 1
_HEADER_SIZE = 16

# Values are on-disk and permanent; they must match DocKind.
_KINDS = {"InstallConfig": 1, "MachineProfile": 2, "Version": 3}


def _frame(kind_name: str, payload: bytes) -> bytes:
    kind = _KINDS.get(kind_name)
    if kind is None:
        _die(f"no document kind for {kind_name}",
             "framing applies to InstallConfig, MachineProfile and Version")
    return (
        _MAGIC
        + bytes([_FRAME_VERSION, kind, 0, 0])
        + struct.pack("<I", len(payload))
        + struct.pack("<I", zlib.crc32(payload) & 0xFFFFFFFF)
        + payload
    )


def _unframe(blob: bytes, expect: str) -> bytes:
    if len(blob) < _HEADER_SIZE:
        _die("not a stored document: shorter than a header")
    if blob[:4] != _MAGIC:
        _die("not a stored document: bad magic")
    if blob[4] != _FRAME_VERSION:
        _die(f"framing version {blob[4]} is not understood")
    declared = struct.unpack("<I", blob[8:12])[0]
    if declared > len(blob) - _HEADER_SIZE:
        _die("truncated: declared payload is longer than the file")
    payload = blob[_HEADER_SIZE:_HEADER_SIZE + declared]
    want = struct.unpack("<I", blob[12:16])[0]
    if (zlib.crc32(payload) & 0xFFFFFFFF) != want:
        _die("payload does not match its CRC")
    kind = blob[5]
    if _KINDS.get(expect) not in (None, kind):
        print(f"note: document kind is {kind}, decoding as {expect}", file=sys.stderr)
    return payload


def cmd_encode(args) -> int:
    msg = _from_json(_message(args.type), sys.stdin.read())
    out = msg.SerializeToString()
    if args.framed:
        out = _frame(args.type, out)
    sys.stdout.buffer.write(out)
    return 0


def cmd_decode(args) -> int:
    blob = sys.stdin.buffer.read()
    if args.framed:
        blob = _unframe(blob, args.type)
    msg = _message(args.type)
    msg.ParseFromString(blob)
    print(_to_json(msg))
    return 0


def cmd_roundtrip(args) -> int:
    """Assert that JSON -> protobuf -> JSON is lossless for a given document.

    The same property the host tests assert in C++, checked against the Python
    implementation — which is a genuinely independent one, so agreement here
    means the schema is unambiguous rather than that one bug is symmetric.
    """
    with open(args.file, "r", encoding="utf-8") as fh:
        original = fh.read()
    msg = _from_json(_message(args.type), original)
    once = _to_json(msg)
    twice = _to_json(_from_json(_message(args.type), once))
    if once != twice:
        print("ROUND TRIP FAILED — re-encoding changed the document", file=sys.stderr)
        return 1
    if json.loads(original) != json.loads(once):
        print("note: input was normalised (defaults filled in); round trip is stable")
    print("round trip stable")
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        prog="pbtool.py",
        description="Talk to a pinled device; convert protobuf <-> canonical JSON.",
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("get", help="GET an endpoint and print it as JSON")
    g.add_argument("url")
    g.add_argument("--type", help="message type (inferred from the URL by default)")
    g.set_defaults(func=cmd_get)

    u = sub.add_parser("put", help="PUT a JSON file to an endpoint as protobuf")
    u.add_argument("url")
    u.add_argument("file")
    u.add_argument("--type", help="message type (inferred from the URL by default)")
    u.set_defaults(func=cmd_put)

    i = sub.add_parser("info", help="shorthand for GET /api/v1/info")
    i.add_argument("url")
    i.set_defaults(func=cmd_info)

    e = sub.add_parser("encode", help="JSON on stdin -> protobuf on stdout")
    e.add_argument("type")
    e.add_argument("--framed", action="store_true",
                   help="wrap in the 16-byte stored-document header, as written to /cfg")
    e.set_defaults(func=cmd_encode)

    d = sub.add_parser("decode", help="protobuf on stdin -> JSON on stdout")
    d.add_argument("type")
    d.add_argument("--framed", action="store_true",
                   help="input is a stored document; verify and strip its header")
    d.set_defaults(func=cmd_decode)

    r = sub.add_parser("roundtrip", help="check a JSON document survives a round trip")
    r.add_argument("type")
    r.add_argument("file")
    r.set_defaults(func=cmd_roundtrip)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
