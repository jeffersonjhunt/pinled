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

## What the fixtures say, and why

`install.json` deliberately does **not** restate the bench rig's geometry. It
changes exactly two things a boot log will show plainly — the LED string is
shortened and the filament defaults are moved — so that "the document was read"
and "the document was ignored" cannot look the same. Everything else is left
absent, which also exercises the inherit path in `install_to_machine()`.

`profile.pb.json` styles four lamps out of a 32-channel install, with one
class-locked and one carrying an explicit decay. Between them they cover the
two rules that matter at boot: a locked channel keeps its own tuning, and an
explicitly chosen value survives profiling whether locked or not.
