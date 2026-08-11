# Host tests

Pure-logic tests that build and run on any machine with a C++17 compiler. No
ESP-IDF, no target, no board attached — which is the point: the schema layer is
ordinary logic and there is no reason to verify it by hand on hardware
(`FIRMWARE_PLAN.md` §5.1 step 1, §6).

```sh
cmake -S test/host -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

Run a single suite directly to see per-case output:

```sh
./build-host/test_doc_frame
```

## What is covered

| Suite | Covers |
|---|---|
| `test_crc32` | CRC-32/ISO-HDLC conformance, incremental use, single-bit-flip detection |
| `test_doc_frame` | stored-document framing: round trip, and every rejection path |
| `test_schema_roundtrip` | protobuf schema through nanopb, against golden bytes from Google's implementation *(needs nanopb — see below)* |
| `test_apply` | precedence between stored configuration and the profiler (FR-CFG-8) |
| `test_doc_file` | documents as real files: round trip, corruption, bounds, atomic replace |
| `test_resolve` | the projection: profile + install → per-channel records, and every case it refuses *(needs nanopb)* |
| `test_install` | the install document → the device-wide record; what a zero means, field by field *(needs nanopb)* |

`test_crc32` asserts the canonical check value `0xCBF43926` for `"123456789"`.
That assertion is doing real work: a reflected/non-reflected mix-up yields a
checksum that is entirely self-consistent — it round-trips, it catches
corruption, every hand-written test passes — and simply is not CRC-32. The
mistake only becomes visible when something outside this codebase has to agree.

`test_doc_frame` is mostly failure modes on purpose. The frame exists because
LittleFS guarantees filesystem consistency and *not* that the last write landed
(`FR-CFG-15`), so the interesting behaviour is what it refuses: truncation, an
over-long declared length, a flipped payload bit, a corrupt CRC field.

`test_doc_file` writes to an actual filesystem, in a `scratch/` directory
relative to the working directory. That is not incidental: `kMaxDocPath` is 96
bytes, and the first version of the suite used an absolute path from CMake and
failed *every* write because a nested build directory exceeds that on its own.
The cap is correct — device paths are `/cfg/install.pb` — so the tests bend, not
the code. Anything new that touches files should do the same.

The one thing it cannot cover is that LittleFS's `rename` on real flash behaves
like POSIX's. That is a property of the medium, and it is checked on the device
by `CONFIG_PINLED_STORE_SELFTEST`.

## Sanitizers are on by default

These are buffer-parsing routines fed deliberately corrupt input, which is
exactly what ASan and UBSan are for. A test that passes while quietly reading
past the end of a truncated document would be worse than no test at all. Turn
them off with `-DPINLED_SANITIZE=OFF` if you need to.

Warnings are errors, including `-Wconversion` and `-Wshadow`.

## Writing a test

`harness.h` is sixty lines rather than a dependency. CTest already provides
discovery, parallelism and reporting, so the only thing missing was `CHECK`.

```cpp
#include "harness.h"

TEST(descriptive_name_of_the_property)
{
    CHECK_EQ(crc32("abc", 3), 0x352441C2u);
}

int main() { return ooe::test::run_all(); }
```

Assertions do **not** abort. One run reports every failure, which matters when a
schema change breaks thirty round-trips and you want the shape of the breakage
rather than the first instance of it. Add the suite to
`test/host/CMakeLists.txt` with `pinled_add_test(name)`.

## Schema round-trips, and why the fixtures are committed

`test_schema_roundtrip` needs nanopb, and is skipped with a message when it is
absent — so a machine without `protoc` still gets a green run for everything
else.

```sh
cmake -S test/host -B build-host -DPINLED_NANOPB_DIR=/path/to/nanopb
```

The `.pb` files under `fixtures/` are encoded by **Google's** protobuf
implementation and decoded here by **nanopb**. That asymmetry is the value:
nanopb agreeing with itself proves very little, whereas nanopb agreeing with the
reference implementation is evidence that `proto/pinled.proto` is unambiguous —
which matters because protobuf.js in the browser has to agree with both
(`FR-CFG-13`).

`nanopb_reencode_matches_googles_bytes` is the strongest assertion here: decode
Google's bytes with nanopb, re-encode, require byte identity. The spec does not
*mandate* that, but both implementations emit in field-number order and omit
proto3 defaults, so a divergence means one of them has changed its mind about
the schema.

The fixtures are committed because they **pin the wire format**. Renumbering a
field or changing a type — the things `proto/pinled.proto`'s header comment
forbids — breaks these tests loudly instead of silently orphaning every document
an older build ever wrote. Verified: renumbering `LampEntry.name` from 2 to 8
fails three cases.

Regenerating a fixture is therefore a decision, not a chore. If one changes and
you did not intend to change the wire format, that is the bug.

```sh
protoc -I proto --python_out=/tmp/pygen proto/pinled.proto
python3 test/host/fixtures/regenerate.py --bindings /tmp/pygen --check   # verify
python3 test/host/fixtures/regenerate.py --bindings /tmp/pygen           # rewrite
```

## Toolchain

Nothing below is needed for `test_crc32` or `test_doc_frame`.

| Need | For |
|---|---|
| `protoc` | generating bindings (`apt install protobuf-compiler`) |
| `pip install protobuf` | fixture regeneration, `pbtool.py` |
| `pip install nanopb` | the generator |
| a nanopb checkout | the C runtime — the pip package ships the generator only |

On the build host (`intel-nuc.tworivers`) this is already set up: `protoc`
3.21.12 system-wide, a venv at `~/.venvs/pinled-tools` with `protobuf` and
`nanopb`, and the runtime cloned at `~/.local/src/nanopb` pinned to 0.4.9.
CMake finds the generator in that venv automatically, so only the runtime path
needs passing:

```sh
cmake -S test/host -B build-host -DPINLED_NANOPB_DIR=$HOME/.local/src/nanopb
```

## Related: talking to a real board

`tools/pbtool.py` is the `curl` replacement — JSON in and out, protobuf on the
wire.

```sh
protoc -I proto --python_out=tools/_gen proto/pinled.proto
tools/pbtool.py get http://pinled.local/api/v1/config
tools/pbtool.py encode MachineProfile < profile.json > profile.pb
```
