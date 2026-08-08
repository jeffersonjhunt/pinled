# Configuration UI, Profiles & Updates — `pinled`

How a user configures a machine: names lamps, assigns colours, tunes fades,
shares the result, and updates firmware. Pairs with `REQUIREMENTS.md`
(`FR-CFG-*`, `FR-UI-*`, `FR-OTA-*`) and `FIRMWARE_PLAN.md` (M3/M4).

**Design intent, stated by the project owner and load-bearing for everything
below:** the cloud exists to allow constant improvement of the *site* without
touching devices in the field, and to be convenient. It must never be
*required*. Every function of the product is reachable with no internet
connection at all.

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

### Install config — never leaves the device

```json
{
  "schema": 1,
  "kind": "install_config",
  "geometry": { "num_modules": 4, "channels_per_module": 16, "led_count": 64 },
  "pins":     { "clk": 18, "pl": 17, "data": 9, "led": 8 },
  "scan":     { "sample_rate_hz": 10000, "spi_hz": 4000000, "spi_mode": 2, "active_low": false },
  "render":   { "refresh_hz": 90, "gamma": 2.2, "brightness_cap": 180 },
  "wiring":   [ { "channel": 0, "lamp": 17, "led_index": 0 } ],
  "wifi":     { "ssid": "…" }
}
```

`wiring[]` is the **join** between the two documents: channel → lamp number →
profile entry. `led_index` belongs here rather than in the profile because the
physical order of the LED string is a property of how this install was built.

Importing a shared profile therefore never touches geometry, pins or wiring. It
repaints lamps the user has already bound.

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

Both documents are JSON files in a LittleFS partition, not NVS blobs. A
128-channel profile with names is roughly 16 KB of JSON; the NVS partition is
24 KB *total*. File storage also delivers `FR-CFG-3` import/export for free —
export is serving the file — and keeps the offline path (§6) working with
ordinary file downloads.

NVS keeps only what must survive a filesystem wipe: Wi-Fi credentials, author
handle, active-profile pointer.

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

Payload is 128 channels × (level byte + flags) at 30 Hz — under 10 KB/s.

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
4. User presses the device button to arm.
5. Browser `POST`s the image to `ota`, streamed to `esp_ota_ops`.

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

### Running with no cloud at all

Every function must remain reachable offline:

| Function | Offline path |
|---|---|
| Provisioning | on-device captive portal, no S3 dependency |
| Configuration | **standalone SPA download**, run locally from `file://` |
| Profiles | plain JSON files, import/export over the device API |
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

> **Not yet applied.** `partitions.csv` is still `factory, 1M` and
> `sdkconfig.defaults` still pins `CONFIG_ESPTOOLPY_FLASHSIZE_4MB`, because the
> bench QT Py is a 4 MB FH4R2 and this table does not fit on it. Both change
> together when the 8 MB board arrives.

> **Bench trap:** the 8 MB no-PSRAM QT Py (FN8C0) is **visually identical** to
> the 4 MB FH4R2 — same silkscreen, same USB VID/PID `239a:8143`. Label them
> physically. `esptool … chip_id` reports embedded flash and PSRAM size and is
> the only reliable discriminator.

## 8. Open questions

1. **Lamp numbering across manufacturers.** The profile is keyed by "lamp
   number in the machine's matrix". Bally/Stern number lamps in the manual;
   whether that generalises cleanly to Williams, Gottlieb and EM games is
   unverified. If it does not, the registry needs a per-manufacturer
   addressing convention, and that affects the schema.
2. **Profile fit checking.** Nothing yet validates that an imported profile's
   `lamp_count` is consistent with the install's `wiring[]`. Decide whether a
   partial match imports partially or is refused.
3. **Multi-machine users.** A device ID exists, but there is no stated model
   for one account owning several devices.
4. **Signed firmware** — deferred, see §5.
