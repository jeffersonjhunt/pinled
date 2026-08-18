# Configuration UI, Profiles & Updates — `pinled`

How a user configures a machine: names lamps, assigns colours, tunes fades,
shares the result, and updates firmware. Pairs with `REQUIREMENTS.md`
(`FR-CFG-*`, `FR-UI-*`, `FR-OTA-*`) and `FIRMWARE_PLAN.md` (M3/M4).

**Design intent, stated by the project owner and load-bearing for everything
below:** the cloud exists to allow constant improvement of the *site* without
touching devices in the field, and to be convenient. It must never be
*required*. Every function of the product is reachable with no internet
connection at all.

> **`ui-mockup.html`** in this directory is a working design reference for what
> follows — open it in a browser, no build step or network needed. The device
> behind it is simulated and none of it is production code, but the channel grid
> runs the same leaky integrator the firmware does, so the reconstruction
> behaves rather than being faked. It exists to make the decisions here
> arguable before they are built, particularly the learn-wiring flow (§4), the
> document split (§3) and OTA arming (§5).
>
> Two things in it are placeholder rather than designed: the install screen is
> read-only, because editing pins and geometry from a web page needs a guard
> rail nobody has specified yet; and lamp numbering is faked as a clean 1–60,
> which is fine as a mockup — §8 question 1 is closed and the numbers are
> opaque, so any machine's own numbering drops straight in.

## 1. Where the code lives

Three pieces, and the split is deliberate:

| Piece | Hosted on | Updated by |
|---|---|---|
| SPA **shell** — a few KB of HTML/JS that boots the app and knows the device API | the **device**, over HTTP | firmware update |
| SPA **bundle** — the actual application, images, help pages, config registry | **S3**, over HTTPS | pushed any time, independently |
| **Profile registry / sharing API** | cloud, over HTTPS | independently |

The browser is the only participant that talks to both the device and the
cloud. **The ESP32 makes no outbound calls to any web service, ever** — not for
updates, not for registration, not for the profile library. It is a LAN device
that serves an API.

### Why the shell must live on the device

This is the one constraint that fixes the whole topology, so it is worth being
explicit about:

- An **HTTPS page cannot fetch HTTP subresources.** Mixed-content blocking is
  unconditional in every modern browser and no CORS header or device setting
  turns it off. So a cloud-hosted SPA at `https://…` could not call
  `http://192.168.1.57/api` — the device would need a real TLS certificate,
  which means per-device cert issuance, an ACME client on the ESP32, and
  renewal infrastructure forever.
- An **HTTP page may fetch HTTPS subresources** freely. That direction has
  never been restricted.

Serving the shell from the device therefore costs ~100 KB of flash and buys
away the entire TLS/certificate problem, while still letting everything heavy
live in S3 and update independently. On an 8 MB part that trade is not close.

### Consequences of an `http://` origin

The device origin is **not a secure context** (the localhost exemption does not
extend to LAN addresses). Two capabilities are unavailable and the SPA must not
be designed around them:

- **No service workers** — the S3 bundle cannot be cached offline that way.
  Ordinary HTTP cache headers only.
- **No `crypto.subtle`** — nothing in the registration or profile path may
  depend on WebCrypto in the browser.

## 2. Device API

Unauthenticated, CORS-open (`Access-Control-Allow-Origin: *`). Open is
required, not merely convenient: the standalone SPA (§6) runs from `file://`
and therefore sends `Origin: null`, which only a wildcard satisfies. There is
no device-side auth to protect, because there is nothing on the device worth
protecting — the machine is on the owner's LAN and the payload is lamp colours.

`/api/v1/…`, JSON except where noted:

| Method | Path | Purpose |
|---|---|---|
| `GET` | `info` | device id, firmware version, **API version**, channel geometry, author handle |
| `GET`/`PUT` | `config` | install config (§3) |
| `GET`/`PUT` | `profile` | machine profile (§3) |
| `GET` | `wifi/scan` | visible SSIDs |
| `PUT` | `wifi` | credentials; device joins and reports its address |
| `WS` | `live` | live per-channel state, ~30 Hz (§4) |
| `GET` | `ota/status` | armed / not armed, current version |
| `POST` | `ota` | firmware image, `application/octet-stream` |

### API versioning is not optional here

Decoupling the S3 bundle from firmware is the main reason for this
architecture, and it has one cost: the bundle moves while a device in the field
may sit on years-old firmware. `GET info` reports an **API version** and the
bundle supports a window backward (N−2 is a reasonable starting rule).

Without this, the first breaking API change strands every un-updated device —
and the update path runs *through* the SPA, so the failure is self-locking:
the tool you would use to fix it is the tool that broke.

## 2a. Wire format: protobuf out, JSON where people read it

A single `.proto` is the **schema authority**. Everything else is generated from
it.

| Where | Format | Why |
|---|---|---|
| Device ⇄ browser | **protobuf** (nanopb) | firmware carries no JSON parser; static structs, no malloc |
| Files, registry, diffs, anything a person reads | **JSON** | human-readable is a product feature for shared profiles (`FR-CFG-3`) |
| The transform | **the browser** | `protobuf.js` `toObject`/`fromObject` |

The JSON form is not a second, hand-maintained schema that can drift from the
first — it is **proto3's canonical JSON mapping**, generated from the same
`.proto` and lossless in both directions. One definition, two encodings.

This is also what makes the API version window (§2) rigorous rather than
hopeful. Field numbers and unknown-field preservation are precisely the
mechanism a cloud-updated bundle needs to talk to firmware from two years ago;
an additive-only JSON convention is a weaker version of the same idea.

On the device this means **nanopb only** — no cJSON, no DOM parse of a 16 KB
document, no string handling on the hot side of a 512 KB SRAM part. A decode
lands directly in the runtime record.

> **The cost, stated plainly:** `curl` stops being a debugging tool, and M3 is
> specifically meant to be exercisable before any UI exists. `protoc --encode`
> and `protoc --decode` in a shell pipeline cover it, and the M3 test harness
> should ship those wrappers rather than leaving each person to work it out.

### Two models, not one

The document model (nested, optional fields, names) and the runtime model
(flat POD array, sized once at init) are **different types**, with `load()`
projecting one into the other. `NFR-4` forbids dynamic allocation in the
per-frame path, and `render_task` touches the per-channel record every frame —
conflating the two is how a `std::string` lamp name ends up dereferenced in the
render loop.

### Integrity is the document's job, not the filesystem's

Every stored document carries a **CRC over its payload**. LittleFS guarantees
filesystem consistency; it does not guarantee that the last write reached
flash. Those are different promises, and the cheap half of the difference is a
checksum field.

### OTA is the one endpoint that needs a gate

Open CORS means any page the user happens to have open can issue requests to
the device. For lamp colours that is a nuisance. For firmware upload it is a
brick. **Arm `POST ota` with a physical button press on the device, valid for
60 seconds**, reported via `ota/status` so the SPA can prompt for it. No
secrets, no pairing, no accounts — physical presence is the whole
authorisation, which fits a device the user is standing in front of.

## 3. Data model: two documents, not one

The single most important schema decision, because profiles are shared and a
mistake here is unfixable once profiles exist in the wild.

**A shared profile must contain nothing install-specific.** Two people with the
same machine may chain their modules in a different order, run a different LED
string length, or be on a different board revision with a different pin map. A
profile carrying channel indices would silently mis-map for the second person;
one carrying pin assignments could break their build outright.

### Machine profile — shareable, this is what the registry hosts

Keyed by **lamp identity** (the lamp's number in the machine's own lamp matrix,
as the operator's manual numbers it), never by channel index.

```json
{
  "schema": 1,
  "kind": "machine_profile",
  "profile_id": "01J8…",
  "revision": 3,
  "machine": { "make": "Bally", "model": "Eight Ball Deluxe", "year": 1981, "lamp_count": 60 },
  "author":  { "handle": "jhunt", "verified": true },
  "lamps": [
    { "lamp": 17, "name": "Left Drop Target", "color": [0, 80, 255],
      "class_lock": "AC_STEADY", "attack_ms": 30, "decay_ms": 40, "gain": 1.0 }
  ]
}
```

### Install config — private, but not device-bound

Never published to the **shared registry**, because it describes one physical
build and would mis-map or mis-configure anyone else's. But it is ordinary user
data: exportable, importable, backed up to the user's **private** cloud area,
and restorable onto a replacement device.

```json
{
  "schema": 1,
  "kind": "install_config",
  "geometry": { "num_modules": 4, "channels_per_module": 16, "led_count": 64 },
  "pins":     { "clk": 18, "pl": 17, "data": 9, "led": 8 },
  "scan":     { "sample_rate_hz": 10000, "spi_hz": 4000000, "spi_mode": 2, "active_low": false },
  "render":   { "refresh_hz": 90, "gamma": 2.2, "brightness_cap": 180 },
  "indicator": { "brightness": 64 },
  "wiring":   [ { "channel": 0, "lamp": 17, "led_index": 0 } ]
}
```

`wiring[]` is the **join** between the two documents: channel → lamp number →
profile entry. `led_index` belongs here rather than in the profile because the
physical order of the LED string is a property of how this install was built.

Importing a shared profile therefore never touches geometry, pins or wiring. It
repaints lamps the user has already bound.

**Wi-Fi credentials are not part of this document** and are never exported, at
any privacy level. They live in NVS only. That is where "never leaves the
device" actually applies — at the field, not at the document. Restoring a backup
consequently requires re-provisioning, which is correct anyway: a restore is
usually onto different hardware or a different network.

## 3a. Versions live off the device

**The device stores exactly one active configuration.** Only one can be in
effect at a time, so a version library on the device would be dead weight in a
partition and a second thing to keep consistent. Versions are managed by the
SPA and stored where the user's other data already lives: their private cloud
area, or plain files on local disk for the offline path.

A **version** is the install config and machine profile captured together, with
a label, a timestamp and parent lineage:

```json
{
  "schema": 1,
  "kind": "version",
  "version_id": "01J8…",
  "label": "warm GI, dimmer inserts",
  "created": "2026-08-08T19:04:11Z",
  "parent": "01J8…",
  "install_config":  { … },
  "machine_profile": { … }
}
```

Pairing the two documents is deliberate: what a user compares is a *look*, and
a look is not confined to one document — insert colours live in the profile,
while gamma, brightness cap and LED order live in the install config. `parent`
records lineage, so an A/B pair reads as two branches from a common point rather
than two unrelated blobs.

Flipping between versions is `PUT config` + `PUT profile` — no special API, no
device-side history.

**Applying a configuration is write-the-file-then-restart.** Nothing in `Main`
is built to be re-initialised in place; it constructs the scan device, the
integrators and the renderer once at boot. Rebuilding that live, while
`scan_task` runs at 10 kHz on core 1, would be real work to solve a problem
nobody has: this is a commissioning-time operation, the lamps flicker for a
second, and the alternative is a teardown path that exists solely to avoid that
second. The UI's obligation is to reconnect its WebSocket cleanly and say what
is happening, rather than appearing to have crashed.

**Fallback is inherent.** The SPA is what applied the change, so it still holds
the outgoing version — going back is one action. No device-side rollback slot is
needed, because **no value in an install config can lock the user out**: pins,
geometry and rates affect scanning and rendering, never the HTTP server. Wi-Fi
credentials are the only setting that could, they are not part of this document
(§3), and they have the button rescue path (FR-UI-7).

### What the shared registry accepts

Only `kind: "machine_profile"`. Versions and install configs go to the user's
private area, which is a backup store, not a publishing surface.

### Per-lamp presentation costs almost nothing

Everything on the presentation list — insert colour, warm-white GI, a softer
fade than the bulb had — is **per-channel state the firmware already owes**:
colour is `FR-LED-4`, and per-lamp fade is per-channel `attack_ms`/`decay_ms`,
which `FR-PROF-3` already requires because the profiler writes exactly those
fields when it classifies a channel.

So presentation is not a new render stage and creates no arbitration problem.
It is the *same* per-channel record, with the UI as a second writer and
`class_lock` deciding who wins. The reconstruction premise in `DOSSIER.md` is
untouched: the UI tints and shapes the reconstructed signal, it does not author
behaviour independent of what the machine is driving.

What is missing today is only the record itself — `MachineConfig` currently
holds ~15 flat scalars and no per-channel array at all, and `LampMapEntry` is
4 bytes of the ~40 needed.

### Storage

Both documents are protobuf files in a LittleFS partition, not NVS blobs. A
128-channel profile with names is roughly 16 KB as JSON; the NVS partition is
24 KB *total*, so the lamp table does not belong there even encoded. NVS keeps
what it is actually good at — the handful of small key-value settings needed at
boot, with its own wear levelling: Wi-Fi credentials and the author handle.

One of each, not a library (§3a).

There is no append-only log and no telemetry, so the storage shape is two
whole-file rewrites at commissioning time. A raw circular partition would be
the better answer for streaming records — it is not the shape of this problem.

## 4. The live view is what makes mapping tractable

Nobody maps 128 channels from a schematic. The `live` WebSocket pushes
per-channel state at ~30 Hz so the user can stand at the playfield, fire a lamp
from the test card (the Alltek Ultimate Test Card does exactly this on demand —
see `DOSSIER.md` §7), and **click the channel that lit**. That builds
`wiring[]` by pointing, and it is the difference between a ten-minute setup and
an afternoon with a manual.

One detail that matters: a 30 Hz snapshot of a 10 kHz scan will miss a lamp
that flashes between two pushes. The device must send a **sticky
"active since last push"** bit per channel, cleared on send, so no activity is
lost regardless of push rate. This mechanism already exists — it is
`FR-DIAG-1`'s sticky `seen_high_` flag, which updates per frame inside
`scan_task`. Reuse it rather than sampling `level[]`.

Payload is 128 channels × (level byte + flags) at 30 Hz — under 10 KB/s. The
frame is a protobuf envelope wrapping a **packed `bytes` field**, not a
`repeated` scalar: the envelope stays versionable like everything else, while
the per-channel data costs two bytes a channel and decodes as a typed array in
the browser.

The push task runs at low priority on core 0; `scan_task` stays pinned to core
1. Backpressure **drops pushes and never stalls the scan** — which the sticky
flag makes harmless, since a dropped push cannot lose activity, only delay it.

There is no broker anywhere in this. MQTT was considered and ruled out by the
architecture's own constraint: a cloud broker means the device dials out
(violating FR-UI-2 and the offline path), and an on-device broker still needs
MQTT-over-WebSocket in the browser — a protocol layer on top of the transport
it was meant to replace.

The same channel also carries live profiler classification, so the user sees
what each channel was classified as and can lock or override it in place.

## 5. Bootstrap and firmware update

### Provisioning

1. Unprovisioned boot → SoftAP `pinled-XXXX` with a captive portal.
2. Self-contained provisioning page — a few KB, **no S3 dependency**: scan
   SSIDs, enter credentials.
3. Device joins the customer network; the portal reports both
   `http://pinled.local` **and the numeric IP**.
4. A button-held rescue path returns to SoftAP when the network changes.

Report the IP as well as the mDNS name. `.local` resolution is reliable on
macOS, Linux and Windows 10+, but flaky in Chrome on Android, and the device
has no display to fall back to.

### Firmware update

The flow, with the device never contacting the cloud:

1. SPA reads the current version from `GET info`.
2. SPA asks the cloud API for the latest version.
3. If newer, the browser `fetch()`es the image from S3 into memory (bucket
   CORS permitting) and verifies a published SHA-256.
4. Browser `POST`s the image to `ota`, streamed straight into the **inactive
   slot**. No arming is needed to upload, because an inactive slot boots
   nothing; the device verifies the app descriptor and computes the SHA-256
   itself as it writes.
5. The device holds the image **staged** and reports `ota_armed` with
   `ota_arm_seconds_left` counting down from ~30. The status pixel blinks
   amber, faster as the window closes.
6. **The user presses the button**, which switches the boot partition and
   reboots. No press, and the staged image is discarded.
7. The new firmware marks itself valid only once it is serving the API; if it
   cannot get that far the bootloader reverts to the previous slot on its own.

**Confirming after the upload rather than arming before it** is the one place
this departs from the obvious design, and it is deliberate (FR-OTA-2). Writing
an inactive slot cannot hurt anything, so the act worth gating is the
boot-partition switch. Arming first would instead open a window during which
*anything* on the LAN may be uploaded, and would happily accept a truncated
image that then has to be rejected at boot. Confirming afterwards binds the
press to one specific image the device has already checked.

That press is the same short press that re-arms the profiler the rest of the
time (FR-OTA-7). One button, three meanings, and the only reason that is safe
rather than a trap is that the indicator says which meaning is live
(FR-IND-3). The long-hold rescue is never modal and works in every state.

Fetching into the browser rather than making the user save and re-select a file
is the same number of moving parts and better UX; the manual save/upload path
stays available for anyone who wants it.

Keep ESP-IDF's app-descriptor check on. Since the binary transits the browser,
the published SHA-256 is the cheap half of integrity; signed images and secure
boot are a separate decision about whether arbitrary firmware on a device the
user owns is a threat worth engineering against.

**USB flashing remains fully supported** as a first-class path, not a recovery
hatch.

## 6. Registration, attribution, and the no-cloud path

Registration exists for **profile ownership and attribution on the sharing
service**, with email verification as the only assurance. Nothing on the device
depends on it.

The device-side surface is correspondingly tiny:

- **Device ID** — derived from the factory eFuse MAC. Unique, stable, needs no
  provisioning step and no secret.
- **Author handle** — optional string in NVS, stamped into exported profiles.

Ownership itself is a cloud-side association between an account, a verified
email, and a `profile_id`. The SPA registers on the user's behalf; the device
is simply where the profile was edited.

The account also owns a **private area** holding the user's version library
(§3a) — their own install configs and profile versions, distinct from the public
registry. This is the *only* version store; the device keeps one active
configuration and no history. Same transport as everything else: the browser
reads from the device and writes to the cloud. The device is not involved and
does not know the area exists.

### Running with no cloud at all

Every function must remain reachable offline:

| Function | Offline path |
|---|---|
| Provisioning | on-device captive portal, no S3 dependency |
| Configuration | **standalone SPA download**, run locally from `file://` |
| Profiles | plain JSON files, import/export over the device API |
| Backup / A-B | versions as JSON files on local disk, applied via the SPA |
| Firmware update | **USB flashing**, or manual `.bin` upload via the SPA |

The standalone SPA is why the device API must answer `Origin: null`. Note that
nothing in the schema changes to support this path — profiles are already
files, which is what makes the offline story fall out rather than needing to be
built twice.

## 7. Flash and partitions

Requires **8 MB** flash. The shipping mainboard carries a bare ESP32-S3, so
this is a BOM choice; it is settled at 8 MB and the decision is far cheaper
made now than after layout.

On 4 MB the layout is possible but has no margin for the life of the product:
two OTA slots at 1.5 MB leave the application no headroom to grow and ~950 KB
for filesystem. At 8 MB:

```
# Name,   Type, SubType,  Offset,   Size
nvs,      data, nvs,      0x9000,   0x6000
otadata,  data, ota,      0xf000,   0x2000
phy_init, data, phy,      0x11000,  0x1000
ota_0,    app,  ota_0,    0x20000,  0x300000
ota_1,    app,  ota_1,    0x320000, 0x300000
storage,  data, spiffs,   0x620000, 0x1E0000
```

3 MB per app slot, 1.875 MB filesystem, exactly filling 8 MB. App partitions
are 64 KB aligned as required. (`spiffs` is the subtype LittleFS uses too.)

> **Applied 2026-08-15**, exactly as written above. `partitions.csv` and
> `sdkconfig.defaults` (`CONFIG_ESPTOOLPY_FLASHSIZE_8MB`) both carry it, and the
> bench runs an FN8C0. The bootloader confirms the layout on every boot, and the
> image sits at ~955 KB — **70% of a slot free**.
>
> It cost a re-provision, as this note predicted it would: the table change
> erases NVS and the filesystem, and the build-time Wi-Fi credentials were
> removed in M3 step 7, so recovery is the captive portal (FR-UI-6). That turned
> out to be worth having — it is the only cheap way to exercise the portal on a
> device that has genuinely never been provisioned, which is the one state it
> exists for. See `BRINGUP.md` §5c for the procedure and its traps.
>
> **This build no longer fits a 4 MB FH4R2** and must not be flashed to one.

> **Bench trap:** the 8 MB no-PSRAM QT Py (FN8C0) is **visually identical** to
> the 4 MB FH4R2 — same silkscreen. Label them physically.
>
> **USB VID/PID does not tell them apart, and an earlier version of this note
> was wrong to say it did.** Measured 2026-08-15: the FN8C0 enumerates as
> `239a:8119` "Adafruit QT Py ESP32-S3 No PSRAM" *while Adafruit's factory
> firmware is still on it*, and as `303a:1001` "Espressif USB JTAG/serial" once
> ours is — as does every other ESP32-S3, including one in ROM download mode. So
> the USB identity describes the running firmware, not the board, and stops
> discriminating the moment you flash it.
>
> `esptool … flash_id` is the check that keeps working: it reports
> `Embedded Flash 8MB` and whether PSRAM appears in `Features`.

## 8. Open questions

1. ~~**Lamp numbering across manufacturers.**~~ **Closed 2026-08-15: it was
   never a schema question.** The indirection already handles it — a private
   `WiringEntry` maps channel to lamp number, a shared `LampEntry` maps lamp
   number to name and colour, and `MachineIdentity` says which machine those
   numbers belong to. `lamp` is an **opaque key**: the firmware only ever tests
   it for zero and for equality, and nothing anywhere parses structure out of
   it. Bally's 1–60, a flattened Williams row/column, and an EM game numbered
   however its manual does it are all just integers.

   Two constraints exist and are already enforced: **0 is reserved for
   "unbound"**, so numbering starts at 1; and **65535 is the ceiling**, because
   the runtime record stores it as `uint16_t` and `resolve()` refuses anything
   larger.

   What made this look urgent was "it affects the schema". It does not: the
   only plausible change is *adding an optional descriptor* recording where a
   numbering came from, and protobuf absorbs an added optional field without
   breaking any document already in the wild. A schema question is only urgent
   when the change would be destructive, and this one cannot be.

   The residual risk is **curation, not structure**: two authors could number
   the same machine differently and their profiles would not transfer. That is
   a registry problem. It is also self-announcing — the learn-wiring flow binds
   against lamps you can watch firing, and a mis-numbered profile shows wrong
   names against the wrong lamps within seconds of being applied.
2. ~~**Profile fit checking.**~~ **Closed 2026-08-15: partial import, and the
   device already does it.** The framing — "partial or refused" — put the
   decision in the wrong layer. `resolve()` iterates *channels*, not profile
   entries, and its policy is already stated in `pinled_resolve.h`: **lenient
   where a half-finished install is normal, strict where a value is ambiguous
   or impossible.**

   | Case | Behaviour |
   |---|---|
   | channel absent from `wiring[]` | allowed — unbound and dark |
   | wired lamp has no profile entry | allowed — bound but unstyled |
   | profile entry for an unwired lamp | ignored — a 60-lamp profile on a 40-lamp install |
   | same channel twice in `wiring[]` | **rejected** |
   | channel or `led_index` out of range | **rejected** |
   | colour > 255, tau or gain > 65535 | **rejected** |

   So the device refuses what it **cannot represent** and honours what it can
   **partially apply**, which is the right split. Refusing an incomplete
   profile would block the case that matters most: someone halfway through
   wiring a playfield who wants the lamp names to help them finish. And
   `lamp_count` is not consulted by the firmware at all — it is metadata for
   the UI, which is the only layer that can do anything useful with it.

   **What is left is therefore a UI reporting decision, not a validation one,
   and it is settled: report the shortfall.** Applying a profile SHALL show how
   much of it landed — "48 of 60 lamps in this profile match your wiring; 12
   unbound" — and SHALL NOT block on it. Partial is the normal state during
   commissioning, and that number is exactly what tells you how far along you
   are, so it doubles as a progress readout rather than a warning. Silence is
   the one unacceptable option, because "I applied the profile and half my
   lamps are white" is otherwise a support call.

   The device is not involved: it has already applied what it could, and the UI
   holds both documents, so the count is arithmetic the browser does. Nothing
   needs adding to the API.

   The asymmetry that makes this safe: an unmatched *profile* entry is inert,
   while an unmatched *wiring* entry lights a lamp with default styling —
   visibly wrong, and fixed by binding it. The failure mode announces itself.
3. **Multi-machine users.** A device ID exists, but there is no stated model
   for one account owning several devices.
4. **Signed firmware** — deferred, see §5.
