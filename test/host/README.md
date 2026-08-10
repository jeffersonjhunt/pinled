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

`test_crc32` asserts the canonical check value `0xCBF43926` for `"123456789"`.
That assertion is doing real work: a reflected/non-reflected mix-up yields a
checksum that is entirely self-consistent — it round-trips, it catches
corruption, every hand-written test passes — and simply is not CRC-32. The
mistake only becomes visible when something outside this codebase has to agree.

`test_doc_frame` is mostly failure modes on purpose. The frame exists because
LittleFS guarantees filesystem consistency and *not* that the last write landed
(`FR-CFG-15`), so the interesting behaviour is what it refuses: truncation, an
over-long declared length, a flipped payload bit, a corrupt CRC field.

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

## Not yet wired: schema round-trips

Tests for document → protobuf → document need nanopb-generated sources, which
need `protoc` plus the nanopb generator. The CMake detects nanopb and skips
those tests when it is absent, so a machine without `protoc` still gets a green
run for everything above.

```sh
cmake -S test/host -B build-host -DPINLED_NANOPB_DIR=/path/to/nanopb
```

They are enabled in step 2, when the document and runtime models exist to
round-trip between (`FR-CFG-14`).

## Related: talking to a real board

`tools/pbtool.py` is the `curl` replacement — JSON in and out, protobuf on the
wire. It needs `protoc` and `pip install protobuf`, neither of which is needed
for the tests here.

```sh
protoc -I proto --python_out=tools/_gen proto/pinled.proto
tools/pbtool.py get http://pinled.local/api/v1/config
```
