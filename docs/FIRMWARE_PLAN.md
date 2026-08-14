# Firmware Plan — `pinled` v2 (ESP-IDF 5.5.x, ESP32-S3)

First-cut architecture for the v2 firmware that replaces the single-mux POC.
Pairs with `REQUIREMENTS.md` (req IDs referenced inline), `TIMING.md` (scan/LED
budgets and bus electrical), and `DOSSIER.md` (design rationale).

Target board: **Adafruit QT Py ESP32-S3**. The scan bus is clocked by SPI + DMA
rather than by software, which is what makes a single fixed sample rate viable
across the whole 8..128 channel range — and, more importantly, what keeps the
clock burst gapless, which the chain requires to hold its bus arbitration.

## 1. Module map

```
main/                         app entry, task wiring, config glue (ooe::pinled::Main)
components/
  lamp_scan/                  chained '165 shift-register scan over SPI → raw per-channel samples
  filament/                   per-channel leaky integrator        → brightness 0..255
  profiler/                   drive-scheme classifier             → per-channel {class,params}
  lamp_map/                   channel → LED index/color + WS2812B (RMT) render
  machine_config/            NVS-backed profiles + Kconfig defaults
```

Dependency direction (no cycles):

```
main ─▶ machine_config
main ─▶ lamp_scan ─▶ (spi_master + DMA)
main ─▶ filament
main ─▶ profiler  ─▶ filament (seeds params)
main ─▶ lamp_map  ─▶ (neopixel/RMT)
```

Each component follows the existing driver-repo layout: `include/<name>.h`,
`<name>.cpp`, `CMakeLists.txt`, and `idf_component.yml` where it has external
deps. Public API in the header, `TAG`-scoped logging, `esp_err_t` returns.

## 2. Runtime model (FreeRTOS)

Two cooperating tasks plus optional profiler, decoupled by a shared brightness
buffer (`uint8_t level[NUM_CHANNELS]`, one writer / one reader — lock-free, the
integrator owns writes):

- **`scan_task`** (pinned to core 1): one receive-only SPI transaction per
  frame — `transmit → unpack 16·N bits → integrator.update(ch, sample)`. Fixed
  **10 kHz/channel regardless of channel count** (FR-SCAN-5/8); at the
  128-channel maximum the DMA burst is 68 µs and the CPU share is 6.4% of one
  core. A fixed rate is what makes the filament time constants mean the same
  thing on a 16-channel bench rig and a 128-channel install, and uniform `dt` is
  what the integrator assumes. The transaction cadence supplies the pacing, so
  there is no separate timer, and `CS` clears the chain between transactions.
- **`render_task`** (60–120 Hz): reads `level[]`, applies `lamp_map` (channel →
  LED index, color, gamma), pushes the WS2812B frame via RMT (FR-LED-1/3).
- **`profiler`** (boot + on-demand): observes raw transitions for a window,
  classifies, writes integrator params, then idles (FR-PROF-*).

Rationale for splitting: the filament integrator *is* the clock-domain crossing.
Sampling fast + rendering slow is exactly the aliasing fix from the dossier, and
it keeps RMT DMA off the hot sample loop.

## 3. Key algorithms

### 3.1 Filament integrator (fixed-point)

Per channel, per sample. Emulates the bulb's thermal low-pass. Two constants so
warm-up and cool-down can differ (real filaments cool slower than they heat, but
starting symmetric is fine):

```
// Q16 fixed point. target = sample ? ONE : 0   (or duty_norm in Q16)
// k_attack / k_decay are per-sample smoothing coeffs derived from tau & Fs:
//     k = 1 - exp(-1 / (tau_seconds * Fs))   → precomputed to Q16 at config time
if (target > level_q16)
    level_q16 += (int64_t)(target - level_q16) * k_attack >> 16;
else
    level_q16 -= (int64_t)(level_q16 - target) * k_decay  >> 16;
out8 = gamma_lut[ level_q16 >> 8 ];   // 0..255
```

No float in the loop (FR-FIL-5); `k_*` and the gamma LUT are computed once when
params change.

### 3.2 Scan driver (`lamp_scan`)

Config: `{spi_host, sclk_pin, data_pin, pl_pin, spi_hz, spi_mode, num_modules,
channels_per_module, active_low}`.

Modules are identical and unaddressed. `CLK` and `/PL` are bussed; `DATA` chains
`QH` → `SER`, so the harness is one shift register 16·N bits deep (FR-SCAN-3).
Raising `/PL` freezes every channel in the chain on one edge; a frame is then
**16·N clocks** with nothing discarded. Protocol in `CHAINING.md`.

The whole frame is one receive-only SPI transaction (FR-SCAN-7, FR-SCAN-10):

```
// Once, at init:
//   SPI mode 2 (CPOL=1, CPHA=0): idle high, sample on falling. A '165 shifts
//   QH on the RISING edge, so the falling edge lands mid bit-cell.
//   CS with SPI_DEVICE_POSITIVE_CS drives /PL: low between frames (registers
//   transparent, loading), high during (frozen, shifting). cs_ena_pretrans
//   sets the /PL-release-to-first-edge gap.

// Per frame:
spi_device_polling_transmit(dev, &{ .length = bits, .rx_buffer = rx });  // bits = 16*N

for (i = 0; i < 16 * num_modules; ++i)
    raw[i] = ((rx[i / 8] >> (7 - (i % 8))) & 1) ^ active_low;
```

Stream bit *i* is channel *i* — no stride arithmetic, no discards. That falls out
of wiring channel 0 to '165 input `H` rather than `A` (`CHAINING.md`); the other
ordering costs a byte-reverse in the hot loop.

Surplus clocks past 16·N are harmless: the chain terminator pulls the far end
low, so extra samples read as lamps-off. Configuring `num_modules` too high or
too low is benign in both directions (FR-SCAN-11).

The chain holds no analog state, so the driver has no timing obligations beyond
the burst — no minimum inter-frame gap, and a stall mid-frame is merely late,
never corrupt. **SPI's single-transaction property is the load-bearing part**
(FR-SCAN-10): the driver costs a measured ~17 µs per transaction irrespective of
length, so a frame must be one transaction and not one per module. At 128
channels that is 49 µs a frame — 49% of a core at 10 kHz, not the 6.4% an
earlier estimate claimed (`TIMING.md` §2.4).

`pl_pin` may be driven either from `CS` (preferred) or as a plain GPIO pulsed
low-then-high before each transaction; the latter is what a bench rig without
`CS` routing uses. Note the idle level inverted between revisions: rev C's `/MR`
idled **high**, rev D's `/PL` idles **low** so the registers sit transparent
between frames. Idling it high instead freezes the chain on whatever it held at
boot and then shifts the terminator's zeros in behind it.

### 3.3 Profiler (classifier)

Over an observation window per channel, accumulate: edge count, high-sample
count (duty), and dominant period (via inter-edge interval histogram). Decision:

```
duty ≈ 1, edges ≈ 0                      → STEADY
periodic bursts, f∈[~200Hz,~2kHz], low duty → MATRIX
period ≈ 8.3/10 ms (2×line), duty ≈ 50%  → AC_STEADY
period ≈ 8.3/10 ms, duty variable         → AC_DIMMED  (conduction angle → gain)
no edges, duty ≈ 0                        → OFF/ABSENT
```

Emit `{class, duty_norm, period_est, confidence}`; map class → integrator
`{gain, k_attack, k_decay}` and normalization. Config profiles can lock a
channel's class (FR-PROF-4). *Stubbed with this algorithm documented in the
first cut; full DSP in v1.*

The observation window is specified in **milliseconds, not frames** (FR-PROF-5).
*(**Done, 2026-08-13**, §5.1 step 8. `PINLED_PROFILE_WINDOW_MS`, default 750,
frames derived from the measured Fs. The first cut observed a fixed 512
frames, which was worse than the 51 ms this paragraph assumed: boot profiling
ran free-run rather than paced, so it was nearer **14 ms** — under two mains
half-cycles.)*

The profiler is also the one part of the pipeline that is *not* robust to
solenoid-induced ground bounce (FR-PROF-6). The filament model absorbs a 1 ms
false-on glitch as a ~3% brightness bump; the classifier sees it as correlated
false edges across every channel at once and will push steady lamps into the
MATRIX bucket. Hence robust statistics, and an easy re-arm path — GPIO 0 (the
QT Py BOOT button) satisfies FR-PROF-2 with no extra hardware. *(The re-arm
landed in §5.1 step 8; the robust statistics have not.)*

### 3.4 Mapping + render (`lamp_map`)

`channel[g] → { led_index, base_rgb, tint, gamma }`. Render multiplies base
color by the channel's brightness, gamma-corrects, optionally dithers low levels
(FR-LED-5), and writes the WS2812B frame. Uses `zorxx/neopixel` (RMT) as the POC
does; `espressif/led_strip` is a swap-in alternative.

Mapping is **1:1 channel-to-LED**, which keeps the output side trivial: 128 LEDs
is `128 × 30 µs + 300 µs` = 4.14 ms, or 50% occupancy at 120 Hz on a single
chain and a single RMT channel. Multi-chain output and parallel RMT transmit are
not needed and are not planned — they would only be required for multi-LED
groups per lamp.

Two things the first cut gets wrong and MVP must fix: the render loop calls
`neopixel_SetPixel()` once per channel, and each call pushes the whole strip
(FR-LED-6) — build the frame, transmit once. And worst-case draw at 128 LEDs is
~6 A, so a global brightness/current cap belongs in the render path rather than
in polish (FR-LED-7).

## 4. Configuration

- **Kconfig** (`main/Kconfig.projbuild`): pins, channel count, modules, default
  time constants, sample/refresh rates, LED count, active polarity — so
  `idf.py menuconfig` sets sane build-time defaults (FR-CFG-2).
- **Filesystem** (`machine_config`): runtime configuration as two JSON files —
  a shareable **machine profile** keyed by lamp number, and a private but
  exportable **install config** holding geometry, pins and wiring
  (FR-CFG-5/6/7). One of each, no version history — versions are paired,
  labelled and managed by the UI off-device (FR-CFG-9/10/11). Boots to Kconfig
  defaults with none stored (FR-CFG-4).
- **NVS**: only what must survive a filesystem wipe — Wi-Fi credentials (which
  never appear in an export, FR-CFG-12) and the author handle.

With 1:1 LED mapping and a single sense bus, the whole topology stays
expressible in Kconfig, so stored profiles remain an M3 item rather than a
prerequisite.

Schema, device API, provisioning and update flows are specified in `WEBUI.md`.
Two of its conclusions constrain code written before M3 and are worth carrying
early: per-lamp presentation is the *same* per-channel record the profiler
writes (so `MachineConfig` needs a per-channel array, which it currently
lacks), and the UI's live view reuses FR-DIAG-1's sticky per-channel flag
rather than sampling `level[]`.

Pin map (QT Py ESP32-S3). **The POC defaults are unusable on this board**:
GPIO 26/27 are SPI flash pins on the S3, and 15/25 are not broken out.

| Signal | GPIO | Board name |
|---|---|---|
| `CLK` (SPI `SCLK`) | 18 | A0 |
| `/PL` (SPI `CS`, positive) | 17 | A1 |
| `DATA` (SPI `MISO`) | 9 | A2 |
| LED string | 8 | A3 |
| Status pixel | 39 (power enable 38, drive high) | onboard NeoPixel |
| Profiler re-arm | 0 | BOOT button |

The sense bus costs three pins, all from one SPI peripheral.

`SCLK` and `MISO` can be routed to any pins through the GPIO matrix; they do not
need to be the default SPI pads. This leaves I2C/STEMMA QT (6/7, 40/41) and the
UART (5/16) free.

The **ESP32-S3-DevKitC-1** is also supported for bring-up and uses the *same
GPIO numbers*, so no firmware change is needed to move between the two — only
the silkscreen labels differ (`IO18`/`IO17`/`IO9`/`IO8` instead of `A0`..`A3`).
Two DevKit-specific cautions: on `N8R8` parts the octal PSRAM occupies GPIO
33–37, so the "SPI free" note above does **not** apply there, and the onboard
RGB LED is on a different pin with no power-enable. Physical positions are in
`HARDWARE.md`.

**Boot-time validation** (FR-SCAN-9, FR-LED-8) — config is checked against
measured reality rather than trusted, so a mis-specified profile fails loudly
instead of quietly corrupting every time constant:

| Check | On failure |
|---|---|
| `num_modules × channels_per_module ≤ 128` | refuse to start |
| measured `t_frame` vs configured Fs | clamp Fs, log, use clamped value for tau |
| `t_led(led_count)` vs `refresh_hz` at 70% occupancy | clamp `refresh_hz`, log |

## 5. Bring-up milestones

1. **M0 — skeleton compiles.** Components register, `Main` boots, version logs,
   LED string shows a heartbeat. *(done)*
2. **M0.5 — retarget to S3.** `IDF_TARGET=esp32s3` in `sdkconfig.defaults` and
   both CMake presets; remap the four GPIOs per §4; confirm `zorxx/neopixel`
   builds on S3. *(done — verified on an ESP32-S3-DevKitC-1: 8 MB flash
   detected, partition table loaded, both tasks running, `active_high`
   confirmed, no watchdog trips. Bring-up also forced `CONFIG_ESP_DEFAULT_CPU_
   FREQ_MHZ_240` per NFR-7, which was silently 160 MHz.)* **Not done:** the
   onboard status pixel is still unlit, and its pin differs between the QT Py
   (39, power-enable 38) and the DevKitC-1 — it needs a board-conditional pin
   or dropping from the milestone.
3. **M1a — real time base.** `lamp_scan` rewritten around SPI + DMA: drop
   the bit-bang loop for one receive-only transaction per frame, unpack 16·N
   bits, and drive `/PL` from `CS`. Add the boot feasibility check that feeds the *measured* Fs to
   `Filament::init()` and `Profiler::init()`. Batch the LED frame into one
   transmit (`lamp_map::render()` currently issues one strip transmit per
   channel, which is FR-LED-6's exact anti-pattern). Needs no chained hardware —
   a single module exercises the whole path. *(**Done and proven, 2026-08-06.**
   Single-transaction SPI path, 16·N unpack with no discards, `/PL` from `CS`,
   10 kHz gptimer pacing, the boot feasibility clamp, and one strip transmit per
   frame — all now measured against real '165s rather than compiled only. Frame
   time 26.8 µs for 32 channels at 4 MHz → 37274 Hz free-run, 27% duty at the
   10 kHz target.)*
4. **M1b — one '165 module on the bench.** Two '165s on a breadboard, 16 test
   inputs, driving LEDs. The three things to settle, in order:
   1. ~~**SPI mode 2**~~ — **PASSED 2026-08-06.** `U1.D` read as channel 4 and
      `U2.D` as channel 12, exactly, at 4 MHz. The exact landing is the proof:
      a mode-3 sample on the shift edge would slide the whole frame by one.
   2. ~~**Bit order**~~ — **PASSED**, same run. Channel 0 is the first bit out,
      on input `H`, and no byte-reverse is needed.
   3. ~~**`/PL` from `CS`**~~ — **PASSED.** The positive-polarity `CS` load
      between frames is confirmed by the rig running on `PL_FROM_CS=y`, and
      holding `/PL` low was confirmed 2026-08-07 with a
      `PINLED_SCAN_HOLD_CH=0` build: `DATA` sat at 0, went to 1 for the ~24 s
      the channel-0 button was held, and returned to 0 on release — one clean
      transition each way, no chatter, no clocking involved. FR-DIAG-3 works.
   ***M1b complete, 2026-08-07.*** *The rig went further than M1b required —
   4× 74HC165 = 2 modules / 32 channels — so the M1c handoff result below came
   free. See `BRINGUP.md` §5.*
5. **M1c — two modules, then eight.** Two modules is the important step: it
   proves the chain handoff, the 10 kΩ terminator, and that unplugging the last
   module blanks its channels to a stable zero rather than noise (HW-11).
   *(**Two-module half complete, 2026-08-06/07** on the 4× '165 rig. Chain
   handoff: channels 16–31 appear in order with no gap. Terminator and live
   unplug: chips 3–4 pulled mid-run held a hard zero across ~4.9 M frames,
   including while the surviving module was actively switching, with `U1.D`
   and `U2.D` still reading channels 4 and 12 as the positive control. HW-11
   confirmed — `num_modules` is a performance setting, not a correctness one.)*
   Then
   scale to 8 / 128 channels, where the remaining risks live: multi-drop `CLK`
   integrity at the far end (`TIMING.md` §4.3 — the new binding constraint),
   clock-skew *direction* (module 8 must be clocked after module 1, HW-14), and
   `/PL` edge quality at 800 mm, since that edge is now the capture instant for
   all 128 channels. Compare measured frame period at 1, 2, 4 and 8 modules
   against §2.4.
6. **M2 — profiler.** Time-based observation window, inter-edge histogram for
   period estimation, AC_DIMMED vs AC_STEADY, ground-bounce robustness. Boot
   classification seeds integrator params; verify a matrixed lamp reads as
   steady-on and a dimmed GI tracks brightness. BOOT button re-arms.
7. **M3 — profiles and the config API.** The headless half of `WEBUI.md`, and
   the part everything else depends on. Ordered so that the parts with no
   hardware dependency come first and the risky part comes before the visible
   one — see §5.1 below.

   **M3 does not need the 8 MB board.** OTA is what needs two 3 MB slots; M3
   needs one app partition and one filesystem, and both fit in 4 MB with room
   to spare (§5.1 step 0). Only M4 waits on new hardware.
8. **M4 — UI and updates.** SPA shell served from the device, bundle in S3
   (FR-UI-1/2), API versioning (FR-UI-4), standalone offline bundle (FR-UI-8),
   browser-mediated OTA with button arming (FR-OTA-1/2/3), device identity and
   author attribution (FR-REG-1). The mapping flow is the acceptance test:
   fire a lamp from the test card and bind it by clicking.
9. **M5 — polish.** Gamma/dither tuning, docs, release BOM.

### 5.1 M3, in order

Sequenced on two principles: the host-testable schema work lands before
anything that needs a board, and the per-channel record — which ripples through
three existing components — lands before the HTTP server, which does not touch
them at all. The visible milestone is last because it is the least risky part.

**Step 0 — partitions, on 4 MB.** The current table stops at `0x110000`, so
~2.9 MB of the bench part is unused. M3 needs no OTA slots:

```
nvs,      data, nvs,     0x9000,   0x6000
phy_init, data, phy,     0xf000,   0x1000
factory,  app,  factory, 0x10000,  2M
storage,  data, spiffs,  0x210000, 1536K     → ends 0x390000, inside 4 MB
```

`CONFIG_ESPTOOLPY_FLASHSIZE_4MB` stays as it is. The 8 MB table in `WEBUI.md`
§7 replaces this at M4. Reflashing the table erases NVS, which currently holds
nothing.

**Step 1 — the `.proto` and the host test rig. *(Done, 2026-08-10.)*** No
target, no board, no IDF.

- `proto/pinled.proto` and `proto/pinled.options` — the schema authority, with
  the compatibility rules that make FR-UI-4's version window work.
- `components/pinled_schema` — CRC-32 and the 16-byte stored-document frame
  (FR-CFG-15), deliberately free of any IDF dependency so the same sources
  build on host and target.
- `test/host` — a 60-line harness rather than a vendored framework; ASan,
  UBSan and `-Werror` (`-Wconversion`, `-Wshadow`) on by default.
- `tools/pbtool.py` — the `curl` replacement that protobuf on the wire costs
  us, verified to produce byte-identical output to the committed fixtures.

**38 cases green** on the build host: 8 CRC, 17 frame, 13 schema. The schema
suite decodes golden bytes produced by *Google's* protobuf implementation using
*nanopb*, which is what makes it a cross-implementation check rather than
nanopb agreeing with itself — protobuf.js in the browser is the third
implementation that must agree. The committed fixtures pin the wire format:
renumbering `LampEntry.name` fails three cases, which is precisely the
protection the `.proto`'s compatibility rules need in order to be more than a
comment.

Both the harness and the fixtures were verified to *fail* when deliberately
broken. A suite that is green by construction is worth nothing.

Toolchain now on `intel-nuc.tworivers`: `protoc` 3.21.12, a venv at
`~/.venvs/pinled-tools` (`protobuf`, `nanopb`), and the nanopb C runtime at
`~/.local/src/nanopb` pinned to 0.4.9. Where nanopb is absent — the dev
container — CMake skips that one suite and the rest still run.

*Deferred to step 2:* the document ⇄ runtime projection tests (FR-CFG-14),
which need the two models to exist before there is anything to project.

**Step 2 — the two models (FR-CFG-14). *(Done, 2026-08-10.)***

The **document model is the generated nanopb struct** — there is deliberately
no hand-written parallel class tree, because a second definition of the same
schema is a second thing to keep in step, which is what one schema authority
(FR-CFG-13) exists to prevent.

The **runtime model** is `ChannelConfig`: 14 bytes, POD, no pointers, no
strings, `static_assert`ed on its size since the render path reads it every
frame. **No lamp names** — nothing in scan or render has ever needed one, and
leaving them out means `GET /profile` can verify the stored CRC and stream the
payload back *without decoding it*, so names never exist as a resident
structure on the device.

`resolve()` joins channel → lamp → profile entry through `install.wiring[]` and
settles every inherit-marker, so nothing downstream learns the convention
exists. Lenient where a half-finished install is normal (unwired channels,
unstyled lamps, profile entries for lamps this install lacks), strict where a
value is ambiguous or impossible (duplicate channels, out-of-range geometry,
unrepresentable colours and time constants).

Also moved `DriveClass` out of `profiler.h` into `pinled_drive_class.h`:
`profiler.h` pulls in `filament.h` and therefore `esp_err.h`, which made the
enum unreachable from anything host-buildable. `static_assert`s now pin it to
the wire enum value-for-value.

**68 cases green** (8 CRC, 17 frame, 13 schema, 30 resolve), and the firmware
still builds for `esp32s3` with the moved enum. Three deliberate breakages
confirmed the new tests fail when they should. Adding `FilamentDefaults` as
field 7 left both golden fixtures byte-identical — checked, not assumed.

*Not in the IDF build yet:* `pinled_channel_config.cpp` needs the generated
nanopb header, and nanopb joins the firmware build at step 4 with the store.
The rest of `pinled_schema` compiles for target today.

**Step 3 — per-channel state through the pipeline (FR-CFG-8). *(Done,
2026-08-10.)***

*This was scoped wrongly in the original plan.* `Filament::set_params(ch, …)`
and `LampMap::set_entry(ch, …)` already existed, and `profile_boot()` already
called the former per channel — so it was never an API change across three
components. The gaps were narrower and more specific:

- **`set_entry` was never called by anything.** `LampMap::init()` calls
  `set_default_mapping()` and that was the end of it, so per-lamp colour had
  nowhere to enter the system.
- **`class_lock` was not honoured.** `profile_boot()` overwrote every channel
  with the classifier's output, locked or not.
- **No path existed** from a resolved `ChannelConfig[]` into the components.

What landed: `pinled_apply` holding the precedence rule as ordinary testable
logic, `Main::apply_channel_config()` as the single place a configuration
becomes running behaviour, and `profile_boot()` now treating the classifier's
answer as a suggestion.

The precedence rule has two halves, and the second is the one that matters: a
locked class means the profiler leaves the channel alone, **and an explicitly
chosen value wins regardless of the lock**. Without that second half a lamp
given a deliberately soft fade would have it silently replaced at the next
boot-time profiling pass — per-lamp presentation would appear to work and then
stop, which is the bug that gets reported as "it forgets".

That also corrected a step 2 mistake: settling the inherit-markers destroyed
the difference between "30 ms because someone asked" and "30 ms because that is
the default", which is exactly what the profiler needs in order to know what it
may overwrite. `ChannelConfig` now carries `ChannelFlags` recording it, at the
cost of two bytes (14 → 16).

Also moved `FilamentParams` out of `filament.h` into `pinled_schema`, mirroring
`DriveClass` in step 2 — the rule deserves ordinary tests, so the type it
operates on has to be reachable off-target.

Reading the rig's boot log before flashing found a real bug that
"behaviour-preserving by construction" had missed: `set_default_mapping()`
carried its own copy of the default tint as three literals, and
`ResolveDefaults` had drifted to a different warm white. Routing boot through
the config path would have silently restyled every unconfigured lamp. Fixed at
the cause — one definition, read by both — with a test pinning them together.

**87 cases green** (8 crc, 17 frame, 17 apply, 13 schema, 32 resolve), and
**verified on hardware**: flashed to the bench QT Py, the boot log diffed
against the pre-step-3 boot is exactly two lines — the intended locked-count
message, and 37334 → 37345 Hz free-run, which is 0.03% measurement jitter with
frame time, Fs and duty unchanged. Two minutes uptime, zero errors, warnings,
panics or overruns.

**Step 4 — LittleFS store (FR-CFG-7/15). *(Done, 2026-08-10.)*** Step 0's
partition table came with it.

The store is written against `<cstdio>`, not an ESP-IDF file API. LittleFS is
reached through VFS, so `fopen("/cfg/install.pb")` is the real interface on the
target — which means the whole of `pinled_doc_file` is host-testable against a
temporary directory, with genuinely truncated and genuinely corrupt files
rather than simulated ones. Only the *mount* is target-specific, and it is
about fifteen lines.

Durability is write-to-`.tmp`, fsync, `rename`. FR-CFG-15 exists because
filesystem consistency and write durability are different promises and LittleFS
only makes the first; the rename is what turns "the write failed" into "the
previous configuration is still there".

**Boot never fails because of storage.** A missing document is the normal state
of a device nobody has configured, and a corrupt one is a fault the user needs
to hear about but not be stranded by; either way boot continues on Kconfig
defaults (FR-CFG-4). Rejected files are logged and **kept** — deleting them
destroys the only evidence, and they are never read again.

`save_document()` takes encoded bytes rather than a `MachineConfig`, because
the device is not the author of its own configuration. Re-encoding from the
runtime record would silently drop every field this firmware does not
understand, which is exactly what the schema's unknown-field preservation
exists to prevent (FR-UI-4).

nanopb joins the IDF build by pointing at the same checkout `test/host` uses,
with the generator run at build time and nothing generated committed. Vendoring
the runtime while still generating would have bought nothing — the generator
stays machine-local either way — so **the firmware now builds only where the
nanopb toolchain is installed**, which is the same requirement the tests have
had since step 1. The day a second machine needs to build it, the fix is to
vendor the runtime *and* commit the generated sources together, with a
regeneration check like `test/host/fixtures/regenerate.py` has.

`pinled_resolve` moved to its own component, `pinled_config`. `pinled_schema`
is reachable from `filament.h` and `profiler.h` and so must link without
nanopb; that constraint used to live in a comment, and a comment is not a build
failure. `MachineConfig` moved into `pinled_schema` as an IDF-free POD for the
same reason `DriveClass` moved in step 2 and `FilamentParams` in step 3.

`install_to_machine()` has one real question in it, asked field by field: what
does a zero mean? proto3 gives scalars no presence, so **submessage presence is
the granularity of "specified"**, and within a present submessage a zero
inherits only where zero cannot be a real value. SPI mode 0 is a real mode;
GPIO 0 is a real pin. An all-zero `Pins` message is the one special case and it
is not arbitrary — four signals cannot share a pin, but a writer that emitted
`Pins` without filling it in is entirely plausible, and honouring it literally
would drive a strapping pin at boot.

**138 cases green** (8 crc, 17 frame, 17 apply, 20 file, 7 colour order,
13 schema, 32 resolve, 24 install). Four deliberate breakages confirmed the new suites fail when they
should: no atomic rename (1), absence folded into an error (1), an all-zero
`Pins` honoured literally (4), `spi_mode` 0 made unreachable (1).

**The adversarial pass found the two worst bugs in the step**, neither of them
in the new code's own logic:

- **A rejected profile still styled lamps.** nanopb leaves whatever it managed
  to decode behind on failure, and `resolve()` was handed the profile whenever
  the *install* was present — rejected or not. The reachable case is exactly
  the one FR-CFG-5 is about: a shared profile from a bigger machine, where
  nanopb fills 128 lamps, hits `max_count` and fails, leaving 128 real entries
  to be applied to this playfield. Confirmed on hardware A/B with a 129-lamp
  document: **2 of 32 channels locked** without the fix, **0** with it, same
  fixture, same boot.
- **Moving the channel count into the store defeated main's own guard.** It
  could only ever see a count that had already been clamped to fit, while the
  oversized geometry went on to `scan_.init()` and `filament_.init()`
  unchecked. Refused now, not truncated.

Also fixed: `mount()` used `format_if_mount_failed` under a comment claiming
that could not destroy a configuration — true only of a filesystem that
mounts. It still formats, because an unmountable config partition must not
leave the device unable to boot the UI that would replace it, but the log now
says the configuration is gone.

**Verified on hardware**, in several flashes, because the fallback path and the
feature are different claims:

| What was flashed | What it proved |
|---|---|
| no documents | mounts, formats, falls back to Kconfig defaults; boot otherwise unchanged |
| `fs_seed` documents | `(stored)`, 12 LEDs from the document, exactly 1 locked channel |
| a byte flipped in `install.pb` | `rejected: bad document (bad crc) — KEPT`, defaults restored, profile warning fired |
| a 129-lamp profile, with and without the fix | 2 locked vs 0 locked — the partial-decode bug, and its repair |
| primaries on the five wired inputs | the strip is RGB-ordered; then all five correct once declared |
| app only, seed off | documents survive an app flash — `/cfg` is not touched |

One thing worth writing down about method: an early run of that A/B showed
"0 locked" for *both* arms, which looked like the bug not existing. It was a
backgrounded flash racing the logger, so the second arm was still running the
first arm's binary. The ELF SHA in the boot log is what settled it, and is the
cheapest way to know which firmware actually answered.

The store selftest (`CONFIG_PINLED_STORE_SELFTEST`) covers the one thing host
tests cannot: that LittleFS's `rename` on real flash behaves like POSIX's. It
passed.

Two bugs came out of hardware rather than tests. `partitions.csv` was the first
flash to change the table, and the config summary said `(stored)` above a
configuration that was entirely defaults — `defaulted()` means *neither*
document is present, which is right for the API and wrong for a line describing
numbers that all come from the install. Found only by corrupting a document on
purpose.

**And the fixture found a rendering bug older than this milestone.** The bench
strip is RGB-ordered, not GRB, so the first saturated colour it was ever asked
to display came out pink. Nothing before step 4 could have revealed it: pinled
had only ever shown its near-white default tint, and grey is byte-identical
through any permutation. Byte order is now `RenderConfig.color_order` in the
install document — additive, `UNSPECIFIED` meaning GRB, so documents written
before the field behave identically — and the packing lives in `pinled_schema`
with its own suite, because `neopixel.c` transmits bits 15:8 *first*, then
23:16, then 7:0, and the byte sent first sits in the middle of the word.
Verified on the bench: five channels, five pure colours, all correct.

**Heap, measured rather than assumed:** the decode buffers peak at **21,388
bytes transient**, all freed before boot completes, leaving **353,404 bytes
free** after init. Two independent measurements agree (368,712 − 21,388 =
347,324, the since-boot minimum). A header comment claiming "roughly 45 KB" was
wrong and is gone. **Image: 0x4e4f0 (317 KB) in a 2 MB partition**, so M4's
Wi-Fi, lwIP and httpd have room in both.

**Step 5 — `esp_http_server` and `/api/v1`. *(Done, 2026-08-11.)***

`GET`/`PUT`/`DELETE` on `/config` and `/profile`, `GET /info`, plus the
networking to reach them: station mode when Kconfig credentials are set,
SoftAP otherwise, and mDNS so `pinled.local` resolves. Both modes now rather
than SoftAP alone, because step 7 needs both anyway and a stopgap with no
successor tends to become the design.

**`GET` returns the stored bytes, unmodified.** The CRC is verified and the
payload copied out without ever being decoded, which is what makes the FR-UI-4
version window real rather than aspirational — a document written by a newer
SPA carries fields this build has no struct for, and re-encoding would drop
every one. It also means `GET /profile` never materialises lamp names on the
device, which is what step 2's two-model split was for. Both behaviours were
observed side by side on the bench: the stored document came back carrying
only geometry, `color_order` and wiring, while the same endpoint on an erased
device synthesised one carrying pins, rates and gamma.

`PUT` decodes **only to validate** and throws the result away; the bytes
stored are the bytes that arrived. An install config is additionally projected
before being accepted, because one that cannot project would leave the device
unbootable-as-configured after the restart — a 400 costs the caller a retry,
accepting it costs a trip to the bench with a USB cable.

CORS is wide open per FR-UI-3, **including on error responses**, and `OPTIONS`
is handled: `application/x-protobuf` is not a CORS-safelisted content type, so
every browser `PUT` preflights. Omit that and writes fail in browsers while
working perfectly from `pbtool`.

Networking never blocks boot. No credentials is the normal state of an
unprovisioned device, not a fault; a join failure is logged and survived. The
lamps are the product and they work with no network at all.

**The bench found a bug that no test could have.** With ESP-IDF's default
`WIFI_PS_MIN_MODEM`, the board answered roughly one packet in ten — mDNS
resolved, the occasional request succeeded at 2 ms, and everything else timed
out. It looked like an httpd fault and was not: ping showed 90% loss below the
HTTP layer. `esp_wifi_set_ps(WIFI_PS_NONE)` took it to **15 of 15 at 2.9 ms
average**, same board, same AP, one line of difference. Modem sleep is wrong
for a mains-powered controller whose job while someone commissions it is to
answer promptly, and the LED string dwarfs anything the radio saves.

Reading the diff afterwards found a worse one: the body-receive loop
`continue`d on timeout without bound, and httpd runs one handler at a time —
so anyone who opened a `PUT`, declared a length and then said nothing would
hang the entire API. Now bounded, verified by holding a connection open with
the body withheld: **408 after 5 s, and `GET /info` still answered 200**.

**Verified on hardware:** `GET /info` (API version 1, device_id from the eFuse
MAC), `GET` on both documents, an `OPTIONS` preflight returning the right
headers, a `PUT` that changed `led_count` and survived the restart, `DELETE`
reverting to build defaults, and both rejection paths — random bytes and a
valid protobuf with impossible geometry — returning **400 with the device
neither restarting nor changing anything**.

**Cost, measured:** image 317 KB → **929 KB** (Wi-Fi, lwIP, httpd and mDNS are
~612 KB of flash), still 56% free in the 2 MB partition. Heap 353 KB → **230
KB free**, so the stack costs ~123 KB of RAM — above the 50–80 KB the plan
guessed, and worth knowing before the WebSocket lands.

*Deferred deliberately:* OTA (`FR-OTA-*`). The endpoint needs button-arming
(FR-OTA-2) and an unarmed bricking endpoint behind open CORS is exactly what
that requirement exists to prevent.

**Step 6 — the live WebSocket (FR-UI-5/9). *(Done, 2026-08-11.)***

`/api/v1/live` carries a `LiveFrame` at ~30 Hz: two bytes a channel, level and
flags, with `DriveClass` in bits 2–4.

**The hand-off is the design.** The scan accumulates activity into plain local
words — **no atomics in the 10 kHz inner loop** — and publishes with four
`fetch_or`s per frame; the push drains with `exchange(0)`. That pairing is what
makes the race benign: a set landing before the drain is reported now, one
landing after survives to the next push. A plain load-then-store on the drain
side would swallow anything set in between, which would present as "the UI
occasionally misses a flash" and never reproduce on demand. `LiveState` lives
in `pinled_schema` so both halves are host-tested, including a two-thread
contention case.

Sticky activity is also what makes FR-UI-9 free: a dropped push is a *longer
window*, not a lost sample. Nothing accumulates and nothing blocks.

**Verified on hardware, with buttons.** All five wired inputs — U1.H, U1.D,
U2.D, U3.D, U4.D, being channels 0, 4, 12, 20 and 28 — appear on the socket
when pressed, and the raw scan's `tog` line agrees independently
(`T---T--- ----T--- ----T--- ----T---`). The level sequence is the strongest
evidence: `59, 173, 225, 244, 251, 253…` then decaying on release, which is the
filament integrator's attack curve sampled at 30 Hz. The whole chain is intact
end to end, not merely the bit.

**Three bugs the bench found, none of which a test would have.**

- **21.4 Hz against a nominal 30.** `vTaskDelay` sleeps for the period *after*
  the work, so the real rate was 1/(work + period). `vTaskDelayUntil` gives
  30.5 Hz with zero sequence gaps over four minutes.
- **LRU socket purging is actively wrong here.** httpd closes the least
  recently used socket when a new one arrives, and a live WebSocket looks
  *idle* to httpd because all its traffic is outbound — so opening a second
  browser tab would have silently killed the first tab's monitor.
- **Discovering a dead client from a failed send is too late.** The OS reuses
  descriptors immediately, so the same fd can already name a different client
  and the "failed" send evicts the newcomer. The log showed exactly that, fd 58
  dropping and reconnecting inside a second. Clients are pruned from httpd's
  `close_fn` now, which is the only reliable moment.

Also a log line that lied: "refusing client 61" immediately followed by
"client 61 connected", with the refused client left holding a socket that would
never carry a frame — indistinguishable from a dead device. It fails the
request now so the socket closes.

Cost: **~5 KB of RAM** (224.8 KB free) and 8 KB of image.

**Open, and the monitor is what exposed it:** every frame reports
`DriveClass = OFF`, for every channel, including ones plainly being pressed.
That is not a packing bug — the profiler runs **once, at boot**, when nothing
was active, so it classified everything as OFF and the socket has faithfully
reported that ever since. The class field is therefore honest and useless: a UI
would paint a working playfield as dead. `FR-PROF-5` anticipates a re-arm path
on GPIO 0, but nothing triggers it and classification never updates while the
machine runs. Commissioning wants "turn the game on, press re-profile, watch
the classes settle" — which means an API endpoint, and belongs with the
profiler work rather than here.

**Step 7 — provisioning (FR-UI-6/7). *(Done, 2026-08-12.)***

**Written directly rather than with `wifi_provisioning`, and the plan above was
wrong about that.** `wifi_provisioning` speaks protocomm — protobuf over a
custom endpoint with an X25519 + AES-CTR handshake, designed for Espressif's
phone app. FR-UI-6 wants a page *self-contained on the device*, so using the
component would have meant writing that entire client in JavaScript, by hand,
with crypto. It saves writing credential exchange and costs far more writing
the thing that talks to it. There is also nothing to protect: the API is
deliberately unauthenticated (FR-UI-3), identity is the factory MAC with no
stored secret (FR-REG-1), and the exchange happens on an AP the user is
standing beside.

What landed instead, on top of step 5's server: `POST /api/v1/provision`
taking a `WifiCredentials` protobuf, `GET /api/v1/scan` listing visible
networks, and a captive portal — DNS hijack plus the probe URLs each OS uses
to decide a network is captive.

**Credentials live in NVS and nowhere else** (FR-CFG-12), in their own
namespace. `InstallConfig` still has no field for them and the `.proto` says
not to add one: two independent barriers, because "remember not to export
this" survives exactly as long as the person who wrote it. They are **not**
encrypted — NVS encryption without secure boot is a lock beside its key — so
treat a board as carrying the credentials of any network it has joined.

The portal page fetches nothing. It hand-rolls two protobuf fields in about
ten lines of JavaScript rather than shipping a schema library to a
microcontroller, and it lists networks from `/scan` because an SSID typed from
memory is the commonest way provisioning fails.

**Rescue** (FR-UI-7) is GPIO 0 held for 5 s on a *running* device — distinct
from holding BOOT at reset, which is ROM download mode. `HARDWARE.md` also
gives that pin to the profiler re-arm step 6 showed we need, so the two are
split by duration and a short press is logged and ignored, deliberately.

Two behaviours worth knowing: a provisioned device that **cannot reach its
network falls back to the SoftAP** rather than sitting unreachable — someone
whose router died needs a way in that is not a USB cable; and the scan is
**de-duplicated by SSID**, because the bench returned four entries for two
networks and a dropdown offering the same name three times asks a question
nobody can answer.

**The build-time credentials are gone**, as their own help text promised.
Verified in the right order: provisioned over the LAN, confirmed the next boot
joined *without* the build-time warning, and only then deleted the options and
reflashed — the board still joins, so NVS is demonstrably the only source.

**157 host cases green**, both golden fixtures byte-identical after the schema
addition, and the `fs_seed` documents checked against their JSON.

**Verified end to end on the bench**, including the half a wired build host
cannot reach: BOOT held five seconds wiped the network and dropped the board
into `pinled-82F6DD`; a phone joined and **the setup page popped by itself**;
both networks appeared in the list; provisioning through the page stored the
credentials, restarted, rejoined, and `pinled.local` answered 200.

**Two bugs, and both were tested-in-the-wrong-state.** The SoftAP came up in
`WIFI_MODE_AP`, and `esp_wifi_scan_start` needs a station interface — so the
scan endpoint worked perfectly in the one state where nobody needs it (already
joined to a LAN) and returned nothing in the only state that matters. Then
`APSTA` alone was not enough: the shared event handler called
`esp_wifi_connect()` on `STA_START`, which now fired for the station interface
that exists purely to make scanning legal, leaving the driver permanently
"connecting" and refusing scans. It connects deliberately now.

Scanning is done **once at boot and cached**, not on demand: a scan hops
channels and the AP cannot follow, so scanning when the page asks would
disconnect the phone that asked.

**And a design gap the bugs exposed:** the page offered a dropdown and nothing
else, so an empty list was a dead end. That was wrong even with the scan
working — a hidden network appears in no scan at all — so manual entry is now
unconditional and the typed name wins when present. The failure was the scan;
the fault was a form with one path through it.

Two things to measure rather than assume along the way: **heap** — Wi-Fi, lwIP
and httpd together are on the order of 50–80 KB and nothing has checked that
against current usage (the FH4R2's 2 MB PSRAM is *not* a safety net, since the
shipping board is specified without it, NFR-8) — and **image size**, which is
what decides whether the 2 MB app partition above is generous or merely
adequate.

**This runs on the bench QT Py, on a branch.** The rig is wired and working and
is not being rebuilt to suit a milestone. `main` stays flashable as the
known-good diagnostic build, and the step-0 table is deliberately compatible
with it — the 2 MB `factory` partition holds the current app with room to
spare, so the table is flashed once and thereafter switching between the two
builds is an ordinary app flash.

**Step 8 — the profiler re-arm (FR-PROF-2, FR-PROF-5).**

Step 6 closed with an open problem: the live monitor reported `DriveClass =
OFF` for every channel, forever, because classification happened once at boot
on a playfield that was switched off. The class field was honest and useless,
and a UI built on it would paint a working machine as dead. This closes it.

**The structural point is that there is now one way to produce a
classification instead of two.** At runtime the scan task owns the chain, so a
re-arm cannot read frames of its own — two readers on one SPI device is not a
thing that works. It has to be fed from that task. Boot now goes the same way,
which is why tasks start *before* the boot pass rather than after it. Two
paths that classify would have drifted, and the one that mattered is the one
that did not exist yet.

Three tasks share it and none of them takes a lock, because each owns a phase
and the state is the baton:

| task | transition |
|---|---|
| requester (httpd or button) | `IDLE → OBSERVING`, by compare-exchange |
| scan | arms, feeds every frame, then `OBSERVING → CLASSIFYING` and notifies |
| main | classifies, applies, `CLASSIFYING → IDLE` |

The compare-exchange makes a second request during a pass a **refusal** rather
than a restart of the window — two people, one pressing the button and one
clicking a UI, should not silently extend how long either waits. And
`Profiler::observe()` ignores frames past the window, which is what lets the
main task read the accumulators without a lock and without stopping the writer:
the counters stop moving on their own.

**The window is milliseconds now** (FR-PROF-5), `PINLED_PROFILE_WINDOW_MS`,
default 750, with the frame count derived from the **measured** Fs like every
other time constant. The old fixed 512 frames was about 14 ms at the boot
free-run rate, which is under two mains half-cycles — it classified
confidently having seen essentially nothing.

`POST /api/v1/profiler` re-arms and `GET` reports progress; a short press of
the button does the same thing. One pin, one task, two jobs split by duration
— and the short press fires on *release*, because at the moment the 300 ms
threshold passes there is no way to tell a short press from the first third of
a long hold, and re-profiling on the way to erasing the network would be a
surprise every time.

**Measured on the bench:** the window runs at a clean 10 kHz throughout
(9.7–10.3 kHz sampled every 70 ms across a full pass) and classification is
**applied 1 ms after the window closes** — request at 18195 ms, classified at
18945, applied at 18946 — so the end-to-end cost of a re-arm is the window
plus nothing.

Two earlier figures for that were both artefacts of how it was measured, and
both are worth recording because they are the same mistake twice.

A reading of 1.1 s was *the measurement*: polling status at 20 Hz starves the
priority-1 main task on core 0, where httpd also lives. A UI polling at 1 Hz
will not see it, and the cost is a late classification rather than a wrong one.

A reading of ~830 ms was **the wrong half of a Kconfig fork**. `SCAN_DEBUG`
had a run loop of its own that slept for 500 ms rather than waiting on the
notification, so a re-arm landed up to half a second after its window. It
defaults to `y` and the bench runs it, so every measurement came from the poll
path while the notification path — the one the design comment described — had
never executed on hardware. The loops are now one loop; `SCAN_DEBUG` sets the
idle timeout and picks the log line and touches the hand-off not at all.

**Also fixed on the way past**, two things neither of which was this step:

The button was started *after* the two early returns in `start_network()`, so
a board that failed to join a network had no rescue button and no re-arm
button — the exact case both exist for. It starts first now, before anything
that can give up.

And **the live monitor's sticky bits accumulated between clients.** The push
task skipped `snapshot()` when nobody was connected, while the scan kept
publishing regardless, so "sticky since the last push" quietly meant "sticky
since the last *reader*" and the first frame of a new session reported
activity that could be minutes old. Found while explaining why an LED was
dark: channel 20 arrived `active` on frame one and quiet on every frame of
the twenty seconds measured afterwards. The push task drains while idle now.
A ghost lamp is a bad first impression and a worse thing to debug — and it
would have put a phantom in step 1 of the bench checklist.

**Still needs the bench, and it is one button-press:** everything above is
mechanism. The claim that *classification tracks what the machine is doing* —
a held channel reading `OFF` before a re-arm and `STEADY` after — cannot be
made from a wired build host, because the inputs are physical. See the
checklist in `BRINGUP.md`.

## 6. Test strategy

**None of this exists yet** — there is not a single test file in the repo. It
is the first thing M3 builds (§5.1 step 1), because the schema layer is pure
logic and there is no excuse for it to be verified by hand on a board.

- **Host unit tests** for `filament` (step response → verify tau), the profiler
  classifier (synthetic waveforms → expected class), and schema round-trips
  (document → protobuf → document, and proto3 JSON both ways). Pure logic with
  no IDF dependency, so plain CMake + CTest, runnable in CI without a target.
  Unity ships with IDF for the on-target cases that genuinely need hardware.
- **On-target** smoke tests: scan a known pattern injected on the mux inputs;
  confirm channel→LED mapping; scope the LED refresh vs. sample rate for
  aliasing.
- **Signal-capture harness** (bench): feed recorded matrix/GI waveforms into the
  front end, confirm reconstruction matches the bulb.

## 7. Notes carried from the POC
- Keeps `ooe::pinled` namespace, `Main` class, Doxygen headers, `TAG` logging,
  `version.h`/`version.txt`, `CMakePresets.json` (v5.5 + v6.0), MIT license.
- POC pin choices (QT Py ESP32 Pico: CLK=GPIO25, RST=GPIO27, DATA=GPIO26,
  NEOPIXEL=GPIO15) are **superseded** — see the §4 pin map. Two of them are SPI
  flash pins on the S3.
- A single-'165/8-channel path remains a valid `channels_per_module=8,
  num_modules=1` configuration — useful for the first bench build.
- The POC's `active_low=true` default is **superseded**. The v2 front end is
  MOSFET (inverting) → Schmitt inverter (inverting) = non-inverting overall, so
  lamp on is logic high (HW-1). The bus bias resistor must be oriented to match
  (HW-2): an undriven net reads *off*, which with a non-inverting front end means a
  pull-down — one per receiving end since rev D.
