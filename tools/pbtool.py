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


def cmd_ota(args) -> int:
    """POST a firmware image; the device stages it and waits for the button.

    The SHA-256 of the file is computed here and sent as X-Image-SHA256, so
    the device can verify what actually arrived (FR-OTA-3). Success means
    STAGED, not applied: the response says how long the button has.
    """
    import hashlib

    url = args.url.rstrip("/")
    if not url.endswith("/ota"):
        url += "/api/v1/ota"

    with open(args.file, "rb") as fh:
        image = fh.read()
    digest = hashlib.sha256(image).hexdigest()
    print(f"pbtool: {len(image)} bytes, sha256 {digest}", file=sys.stderr)

    req = urllib.request.Request(url, data=image, method="POST")
    req.add_header("Accept", CONTENT_TYPE)
    req.add_header("Content-Type", "application/octet-stream")
    req.add_header("X-Image-SHA256", digest)
    try:
        # An upload over Wi-Fi to a flash-erasing device is slow; 120 s is
        # generous without being infinite.
        with urllib.request.urlopen(req, timeout=120) as resp:
            body = resp.read()
    except urllib.error.HTTPError as exc:
        detail = ""
        try:
            result = _message("ApplyResult")
            result.ParseFromString(exc.read())
            detail = f": {result.message}"
        except Exception:
            pass
        _die(f"POST {url} -> HTTP {exc.code} {exc.reason}{detail}")
    except urllib.error.URLError as exc:
        _die(f"cannot reach {url}: {exc.reason}", "is the board on the network?")

    result = _message("ApplyResult")
    result.ParseFromString(body)
    print(_to_json(result))
    return 0


def cmd_author(args) -> int:
    """Store or clear the author handle (FR-REG-1). No restart either way."""
    url = args.url.rstrip("/")
    if not url.endswith("/author"):
        url += "/api/v1/author"
    if args.clear:
        body = _http(url, method="DELETE")
    else:
        if args.handle is None:
            _die("give a handle, or --clear")
        msg = _message("AuthorHandle")
        msg.handle = args.handle
        body = _http(url, data=msg.SerializeToString(), method="PUT")
    result = _message("ApplyResult")
    result.ParseFromString(body)
    print(_to_json(result))
    return 0


def cmd_colortest(args) -> int:
    """The colour lab: pin one LED to exact bytes, live (no restart)."""
    url = args.url.rstrip("/")
    if not url.endswith("/colortest"):
        url += "/api/v1/colortest"
    if args.clear:
        body = _http(url, method="DELETE")
    else:
        if args.led is None or args.r is None:
            _die("give LED R G B, or --clear")
        msg = _message("ColorTest")
        msg.led = args.led
        msg.color.r, msg.color.g, msg.color.b = args.r, args.g, args.b
        msg.level = args.level
        msg.raw = args.raw
        msg.gamma_x100 = args.gamma
        body = _http(url, data=msg.SerializeToString(), method="POST")
    result = _message("ApplyResult")
    result.ParseFromString(body)
    print(_to_json(result))
    return 0


def cmd_ota_discard(args) -> int:
    url = args.url.rstrip("/")
    if not url.endswith("/ota"):
        url += "/api/v1/ota"
    body = _http(url, method="DELETE")
    result = _message("ApplyResult")
    result.ParseFromString(body)
    print(_to_json(result))
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


# --- the live socket ----------------------------------------------------------
#
# A ~60-line WebSocket client rather than a dependency, for the same reason
# harness.h is sixty lines rather than Catch2: the only thing needed is "open
# it, read binary frames", and `pip install websockets` is a network fetch on a
# machine that may not have one.
#
# Client-to-server frames must be masked (RFC 6455 §5.3) but we never send any
# except the close, so the masking here is minimal and deliberate.


def _ws_connect(url: str, timeout: float = 5.0):
    import base64
    import os as _os
    import socket
    from urllib.parse import urlparse

    u = urlparse(url)
    host = u.hostname or ""
    port = u.port or (443 if u.scheme == "wss" else 80)
    if u.scheme == "wss":
        _die("wss is not supported", "the device speaks plain HTTP on the LAN (FR-UI-1)")
    path = u.path or "/"

    key = base64.b64encode(_os.urandom(16)).decode()
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.sendall(
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n".encode()
    )

    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(1024)
        if not chunk:
            _die(f"{url} closed during the handshake")
        buf += chunk
    status = buf.split(b"\r\n", 1)[0].decode(errors="replace")
    if "101" not in status:
        _die(f"{url} refused the upgrade: {status}")
    return sock, buf.split(b"\r\n\r\n", 1)[1]


def _ws_frames(sock, initial: bytes = b""):
    """Yield (opcode, payload) for each frame. Server frames are unmasked."""
    buf = bytearray(initial)

    def need(n: int) -> bytes:
        while len(buf) < n:
            chunk = sock.recv(4096)
            if not chunk:
                raise ConnectionError("socket closed")
            buf.extend(chunk)
        out = bytes(buf[:n])
        del buf[:n]
        return out

    while True:
        h = need(2)
        opcode = h[0] & 0x0F
        masked = bool(h[1] & 0x80)
        length = h[1] & 0x7F
        if length == 126:
            length = int.from_bytes(need(2), "big")
        elif length == 127:
            length = int.from_bytes(need(8), "big")
        mask = need(4) if masked else b""
        payload = need(length)
        if masked:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        yield opcode, payload


# Bits 2..4 of each channel's flag byte, as DriveClass. The letters are
# chosen to be told apart at a glance in a 128-character row: '.' for a
# channel the profiler called OFF, so a row of mostly-dots reads as a
# playfield that was dark when it was last classified.
#
# EIGHT entries for a three-bit field, not six for the six classes that
# exist. A newer firmware with a seventh class -- or one flipped bit -- would
# otherwise index past the end and kill the watcher with an IndexError, which
# is a rotten way to find out you are talking to a device you do not know.
_CLASS_LETTERS = "?.SMAD!!"
_CLASS_NAMES = {"?": "unknown", ".": "off", "S": "steady", "M": "matrix",
                "A": "ac_steady", "D": "ac_dimmed", "!": "unrecognised"}


def cmd_live(args) -> int:
    """Watch the live monitor and print one line per push."""
    import time

    sock, extra = _ws_connect(args.url)
    msg = _message("LiveFrame")

    started = time.monotonic()
    count = 0
    last_seq = None
    gaps = 0
    last_row = None

    try:
        for opcode, payload in _ws_frames(sock, extra):
            if opcode == 0x8:  # close
                print("server closed the socket")
                break
            if opcode not in (0x2, 0x1):
                continue

            msg.Clear()
            msg.ParseFromString(payload)
            count += 1

            if last_seq is not None and msg.seq != last_seq + 1:
                gaps += 1
            last_seq = msg.seq

            samples = msg.samples
            active, bound = [], 0
            for ch in range(msg.channel_count):
                if ch * 2 + 1 >= len(samples):
                    break
                level, flags = samples[ch * 2], samples[ch * 2 + 1]
                if flags & 0x01:
                    active.append((ch, level, (flags >> 2) & 0x07))
                if flags & 0x02:
                    bound += 1

            if args.classes:
                # One row per push, one letter per channel. This is the view
                # that makes a profiler re-arm visible: activity comes and
                # goes at 30 Hz and is hard to read, whereas the class is a
                # standing answer that only changes when a pass completes.
                # Printed only when it CHANGES, so a re-arm is one new line
                # in an otherwise still terminal.
                row = "".join(
                    _CLASS_LETTERS[(samples[ch * 2 + 1] >> 2) & 0x07]
                    if ch * 2 + 1 < len(samples) else "?"
                    for ch in range(msg.channel_count))
                if row != last_row:
                    counts = {c: row.count(c) for c in sorted(set(row))}
                    tally = " ".join(f"{_CLASS_NAMES.get(c, c)}={n}"
                                     for c, n in counts.items())
                    print(f"seq {msg.seq:6d}  [{row}]  {tally}")
                    last_row = row
            elif args.verbose or active:
                shown = " ".join(f"ch{c}:lvl{l}:cls{k}" for c, l, k in active[:8])
                print(f"seq {msg.seq:6d}  {len(payload):4d}B  "
                      f"{msg.channel_count} ch, {bound} bound  {shown}")

            if args.seconds and time.monotonic() - started >= args.seconds:
                break
    except (ConnectionError, KeyboardInterrupt) as exc:
        print(f"stream ended: {exc}", file=sys.stderr)
    finally:
        sock.close()

    elapsed = max(time.monotonic() - started, 1e-6)
    print(f"\n{count} frames in {elapsed:.1f}s = {count / elapsed:.1f} Hz; "
          f"{gaps} sequence gap(s)")
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

    o = sub.add_parser("ota", help="POST a firmware image; staged until the button confirms")
    o.add_argument("url", help="device base URL, or .../api/v1/ota")
    o.add_argument("file", help="the .bin image (build/pinled.bin)")
    o.set_defaults(func=cmd_ota)

    od = sub.add_parser("ota-discard", help="discard a staged image (DELETE /api/v1/ota)")
    od.add_argument("url")
    od.set_defaults(func=cmd_ota_discard)

    ct = sub.add_parser("colortest", help="pin one LED to exact bytes, live (the colour lab)")
    ct.add_argument("url")
    ct.add_argument("led", nargs="?", type=int)
    ct.add_argument("r", nargs="?", type=int)
    ct.add_argument("g", nargs="?", type=int)
    ct.add_argument("b", nargs="?", type=int)
    ct.add_argument("--level", type=int, default=0, help="0..255 output scale (default full)")
    ct.add_argument("--raw", action="store_true", help="bypass the sRGB->linear conversion")
    ct.add_argument("--gamma", type=int, default=0,
                    help="gamma x100 to try (160 = x^1.6); 0 = the device's configured gamma")
    ct.add_argument("--clear", action="store_true", help="remove the override")
    ct.set_defaults(func=cmd_colortest)

    au = sub.add_parser("author", help="store or clear the author handle (FR-REG-1)")
    au.add_argument("url", help="device base URL, or .../api/v1/author")
    au.add_argument("handle", nargs="?", help="the handle to store")
    au.add_argument("--clear", action="store_true", help="forget the stored handle")
    au.set_defaults(func=cmd_author)

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

    lv = sub.add_parser("live", help="watch the live monitor WebSocket")
    lv.add_argument("url", help="ws://<host>/api/v1/live")
    lv.add_argument("--seconds", type=float, default=5.0,
                    help="stop after this long (0 = run until interrupted)")
    lv.add_argument("--verbose", action="store_true",
                    help="print every frame, not only those with activity "
                         "(ignored with --classes, which prints on change)")
    lv.add_argument("--classes", action="store_true",
                    help="one letter per channel's DriveClass, printed only "
                         "when it changes -- the view for watching a "
                         "POST /api/v1/profiler land")
    lv.set_defaults(func=cmd_live)

    r = sub.add_parser("roundtrip", help="check a JSON document survives a round trip")
    r.add_argument("type")
    r.add_argument("file")
    r.set_defaults(func=cmd_roundtrip)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
