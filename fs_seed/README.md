# `fs_seed` — a filesystem image for bring-up

Two stored documents (built into `image/`), flashed into the `storage` partition when
`CONFIG_PINLED_SEED_FS` is on. It exists to answer a question the host tests
cannot: does the device read a document that something *else* wrote?

Everything else about the store is verified off-target. This is the seam where
the C, nanopb, and Google's protobuf have to agree about the same bytes on the
same flash — so the documents here are authored on the host with `pbtool.py`,
exactly as the SPA will author them, and the device only ever reads them.

**Off by default, and it must stay that way.** With it on, every `idf.py flash`
overwrites the device's configuration with these files. That is what you want
for bring-up and never what you want afterwards.

## Regenerating

```sh
protoc -I proto --python_out=tools/_gen proto/pinled.proto
tools/pbtool.py encode InstallConfig  --framed < fs_seed/install.json  > fs_seed/image/install.pb
tools/pbtool.py encode MachineProfile --framed < fs_seed/profile.pb.json > fs_seed/image/profile.pb
```

`--framed` adds the 16-byte header from `pinled_doc_frame.h`, computed by a
second implementation of it in Python. If that ever disagrees with the C, the
device rejects the document on a CRC — which is the check working.

The `.pb` files are committed so that flashing the image needs no `protoc`.
That means **the JSON and the `.pb` are two copies of the same thing and can
drift** — and did, immediately: the first edit to `install.json` was committed
without regenerating, so a clean checkout would have flashed the previous
fixture. Editing the JSON without rerunning the commands above is the whole
failure mode. Verify with:

```sh
tools/pbtool.py encode InstallConfig  --framed < fs_seed/install.json    | cmp - fs_seed/image/install.pb
tools/pbtool.py encode MachineProfile --framed < fs_seed/profile.pb.json | cmp - fs_seed/image/profile.pb
```

Regenerating also needs current Python bindings — `protoc -I proto
--python_out=tools/_gen proto/pinled.proto`. Stale ones fail loudly (an
unknown field), which is the one part of this that cannot go wrong quietly.

## What the fixtures say, and why

The wiring matches **the five inputs actually connected on the bench rig** —
`U1.H`, `U1.D`, `U2.D`, `U3.D`, `U4.D`, which are channels 0, 4, 12, 20 and 28
(`HARDWARE.md`: channel 0 is input `H`, counting *down* to channel 7 on `A`).
A fixture that maps channels nothing is wired to cannot be checked by pressing
anything, which is how the first version of this went wrong — it mapped eight
channels, only one of which had a wire on it.

Each gets a **pure primary** on its own LED, because that is what makes the
strip's byte order readable by eye:

| channel | LED | asked for |
|---|---|---|
| 0 | 0 | red |
| 4 | 1 | green |
| 12 | 2 | blue |
| 20 | 3 | white, class-locked to AC_STEADY |
| 28 | 4 | amber |

If a channel lights the colour named, the order is right. If red comes out
green, red and green are swapped — an RGB-ordered strip against a GRB driver.
A near-white default tint hides that completely, which is why it went unnoticed
until the first saturated colour was ever displayed.

Lamp 4 carries a class lock so the boot log's "N of 32 channels locked" line
still shows the profile being applied without anyone pressing anything.
