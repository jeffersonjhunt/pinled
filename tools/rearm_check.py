#!/usr/bin/env python3
"""Acceptance test for the profiler re-arm (FR-PROF-2), BRINGUP.md section 5b.

Everything about the re-arm except one claim can be checked from a wired build
host, and this script checks all of that automatically. The claim that cannot is
**that classification tracks what the machine is doing**, which needs a channel
physically held while a pass runs. So the script drives the API, watches the
live socket, and stops to ask for the four presses that need a person.

    tools/rearm_check.py http://pinled.local

The two steps that matter most are the ones that look redundant:

  * releasing and re-arming, because a class that only ever moves one way would
    pass every other step while being a latch rather than a classifier; and
  * the locked channel, because a re-arm that ignored the lock table would also
    pass every other step, while quietly overwriting every hand-set lamp on a
    playfield.

Exits non-zero if any step fails, so it can be used as a gate rather than read.
"""

import argparse
import os
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pbtool  # noqa: E402

# DriveClass, as packed into bits 2..4 of each channel's flag byte. Eight
# entries for a three-bit field: an unrecognised value must render, not raise.
CLASS = ["unknown", "off", "steady", "matrix", "ac_steady", "ac_dimmed", "?6", "?7"]
LIVE_ACTIVE = 0x01


class Bench:
    """The live socket plus the profiler endpoint, as one thing to talk to."""

    def __init__(self, base):
        self.base = base.rstrip("/")
        ws = "ws://" + self.base.split("://", 1)[-1] + "/api/v1/live"
        self.sock, extra = pbtool._ws_connect(ws)
        self.sock.settimeout(2.0)
        self.frames = pbtool._ws_frames(self.sock, extra)
        self.msg = pbtool._message("LiveFrame")

    def sample(self):
        """Next live frame as (classes, active) lists, one entry per channel."""
        for opcode, payload in self.frames:
            if opcode not in (0x1, 0x2):
                continue
            self.msg.Clear()
            self.msg.ParseFromString(payload)
            s = self.msg.samples
            n = self.msg.channel_count
            classes = [CLASS[(s[c * 2 + 1] >> 2) & 0x07] for c in range(n)]
            active = [bool(s[c * 2 + 1] & LIVE_ACTIVE) for c in range(n)]
            return classes, active
        raise RuntimeError("live socket ended")

    def drain(self, seconds):
        """Read and discard, so a later sample is not a stale queued frame."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.sample()

    def rearm(self):
        """POST a re-arm. Returns the HTTP status; 409 means one is running."""
        req = urllib.request.Request(self.base + "/api/v1/profiler",
                                     data=b"", method="POST")
        try:
            return urllib.request.urlopen(req, timeout=5).status
        except urllib.error.HTTPError as e:
            e.read()
            return e.code

    def await_class(self, ch, want, timeout=4.0):
        """Wait for channel `ch` to classify as `want`. Returns (ok, seen)."""
        end = time.monotonic() + timeout
        seen = None
        while time.monotonic() < end:
            classes, _ = self.sample()
            seen = classes[ch]
            if seen == want:
                return True, seen
        return False, seen

    def held_active(self, ch, seconds=1.0):
        """True if `ch` reports activity at all during `seconds`."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            _, active = self.sample()
            if active[ch]:
                return True
        return False


results = []


def check(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f"   {detail}" if detail else ""))
    return ok


def ask(prompt):
    print(f"\n>>> {prompt}")
    input("    press Enter when done: ")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("base", nargs="?", default="http://pinled.local",
                   help="device base URL (default: http://pinled.local)")
    p.add_argument("--free", type=int, default=4,
                   help="an unlocked wired channel to exercise (default 4, U1.D)")
    p.add_argument("--locked", type=int, default=20,
                   help="a channel the stored profile locks (default 20, U3.D)")
    p.add_argument("--locked-class", default="ac_steady",
                   help="the class that channel is locked to")
    p.add_argument("--include-rescue", action="store_true",
                   help="also hold the button past the erase threshold. DESTRUCTIVE: "
                        "wipes the stored network and reboots into SoftAP, so the "
                        "device must be re-provisioned afterwards.")
    args = p.parse_args()

    b = Bench(args.base)
    free, locked = args.free, args.locked
    print(f"watching {args.base}  (free channel {free}, locked channel {locked})\n")

    # 1 -- the boot pass saw a dark playfield.
    classes, _ = b.sample()
    check("1. boot classification is mostly off",
          classes.count("off") >= len(classes) - 2,
          f"{classes.count('off')}/{len(classes)} off, ch{free}={classes[free]}")
    check(f"1b. locked ch{locked} already reads {args.locked_class}",
          classes[locked] == args.locked_class, f"got {classes[locked]}")

    # 2 -- a held input is SENSED but not RECLASSIFIED. Both halves matter: the
    # first proves the press is real, the second is the staleness being fixed.
    ask(f"HOLD channel {free} down and keep holding it")
    b.drain(0.5)
    check(f"2a. ch{free} is sensed as active while held", b.held_active(free))
    classes, _ = b.sample()
    check(f"2b. ch{free} class is still stale without a re-arm",
          classes[free] == "off", f"got {classes[free]}")

    # 3 -- re-arm while held.
    code = b.rearm()
    check("3a. POST /api/v1/profiler accepted", code == 200, f"HTTP {code}")
    ok, seen = b.await_class(free, "steady")
    check(f"3b. ch{free} reclassifies to steady while held", ok, f"got {seen}")

    # 6 -- the locked channel must NOT have moved in that same pass.
    classes, _ = b.sample()
    check(f"6. locked ch{locked} untouched by that pass",
          classes[locked] == args.locked_class, f"got {classes[locked]}")

    # 4 -- and back again. Without this a latch passes everything above.
    ask(f"RELEASE channel {free}")
    b.drain(0.5)
    code = b.rearm()
    check("4a. POST accepted", code == 200, f"HTTP {code}")
    ok, seen = b.await_class(free, "off")
    check(f"4b. ch{free} returns to off once released", ok, f"got {seen}")

    # 5 -- the same thing from the button rather than the API.
    ask(f"HOLD channel {free} again, CLICK the BOOT button (a normal press; "
        f"anything under 5 s), and KEEP HOLDING channel {free}")
    ok, seen = b.await_class(free, "steady", timeout=6.0)
    check("5. a short button press re-arms too", ok, f"got {seen}")

    # 7 -- a second request during a pass is refused, not queued.
    ask(f"RELEASE channel {free}")
    b.drain(0.5)
    first = b.rearm()
    second = b.rearm()
    check("7. a second re-arm during a pass is refused",
          first == 200 and second == 409, f"first {first}, second {second}")
    b.drain(1.5)  # let that pass finish before anything else

    # 8 -- the long hold. Non-destructive by default: the countdown proves the
    # long-hold branch is entered, and the erase itself is unchanged from M3.
    if args.include_rescue:
        ask("HOLD the BOOT button for a full 6 s. This ERASES the stored network "
            "and reboots into SoftAP -- you will have to re-provision")
        print("    (not asserted here: the device has gone off the network)")
    else:
        print("\n  skipped: 8. long-hold rescue (pass --include-rescue; it wipes "
              "the stored network)")

    failed = [n for n, ok, _ in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    if failed:
        print("failed: " + ", ".join(failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
