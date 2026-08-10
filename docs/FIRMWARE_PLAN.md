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
The first cut observes a fixed 512 frames, which at 10 kHz is 51 ms — about
three AC cycles, far too short to classify AC drive. Budget 500 ms–1 s and
derive the frame count from the measured Fs.

The profiler is also the one part of the pipeline that is *not* robust to
solenoid-induced ground bounce (FR-PROF-6). The filament model absorbs a 1 ms
false-on glitch as a ~3% brightness bump; the classifier sees it as correlated
false edges across every channel at once and will push steady lamps into the
MATRIX bucket. Hence robust statistics, and an easy re-arm path — GPIO 0 (the
QT Py BOOT button) satisfies FR-PROF-2 with no extra hardware.

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

**Step 1 — the `.proto` and the host test rig. *(Partly done, 2026-08-10.)***
No target, no board, no IDF. `filament` and the whole schema layer are pure
logic, so they build with plain CMake + CTest and run in CI.

*Done:* `proto/pinled.proto` and `proto/pinled.options` (the schema authority,
with the compatibility rules that make FR-UI-4's version window work);
`components/pinled_schema` carrying CRC-32 and the 16-byte stored-document
frame (FR-CFG-15), deliberately free of any IDF dependency so it builds on
both host and target; `test/host` with a 60-line harness, ASan/UBSan and
`-Werror` on by default — **25 cases green**, 8 for the CRC and 17 for the
frame, most of the latter being rejection paths. `tools/pbtool.py` is the
`curl` replacement (JSON in and out, protobuf on the wire).

*Remaining:* generating the nanopb and protobuf.js sides, and the round-trip
tests through them. Both need `protoc` plus the nanopb generator, and the
round-trip has nothing to round-trip *between* until step 2 defines the
document and runtime models — so the CMake detects nanopb and skips those
tests when it is absent, keeping the rest green on a machine without `protoc`.

**Step 2 — the two models (FR-CFG-14).** The document type, the flat runtime
record, and the projection between them. Host-testable.

**Step 3 — per-channel state through the pipeline (FR-CFG-8).** The one that
ripples: `filament`, `lamp_map` and `profiler` all take *global* attack, decay
and gamma today and must take them per channel. Largest diff in the milestone
and the one most likely to disturb behaviour already proven on the bench — so
it lands while the rig is still set up to notice.

**Step 4 — LittleFS store (FR-CFG-7/15).** Replace the `machine_config` stub
(`save()` currently returns `ESP_ERR_NOT_SUPPORTED`). Two documents, CRC per
document, boot to Kconfig defaults when absent (FR-CFG-4).

**Step 5 — `esp_http_server` and `/api/v1`.** Config and profile read/write,
`info` reporting the API version (FR-UI-4). Exercised through the step-1
wrappers.

**Step 6 — the live WebSocket (FR-UI-5/9).** Needs `CONFIG_HTTPD_WS_SUPPORT`.
The push task reads the sticky flags `scan_task` already maintains per frame;
define that hand-off explicitly rather than relying on `uint8_t` writes being
atomic. Low priority, off core 1, drops under load.

**Step 7 — provisioning (FR-UI-6/7).** Take ESP-IDF's `wifi_provisioning`
component rather than writing credential exchange by hand; it assumes a
companion app, so pair it with the DNS-hijack captive portal from IDF's own
`protocols/http_server` example. Button-held rescue back to SoftAP.

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
