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
  lamp_scan/                  chained '161 + dual '251 scan over SPI → raw per-channel samples
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
  128-channel maximum the DMA burst is 64 µs and the CPU share is 6.4% of one
  core. A fixed rate is what makes the filament time constants mean the same
  thing on a 16-channel bench rig and a 128-channel install, and uniform `dt` is
  what the integrator assumes. The transaction cadence supplies the pacing, so
  there is no separate timer; the gap between transactions is also the chain's
  frame reset and must stay above 5τ.
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

Config: `{spi_host, sclk_pin, data_pin, spi_hz, num_modules, channels_per_module,
active_low}`. Note there is no `mr_pin` — the chain has no reset conductor.

Modules are identical and unaddressed; each forwards the clock only once it has
scanned its own 16 lines, so the clock is the token and the master just issues
16·N pulses (FR-SCAN-3). Protocol in `CHAINING.md`.

The whole frame is one receive-only SPI transaction (FR-SCAN-7):

```
// Once, at init:
//   SPI mode 0 (CPOL=0, CPHA=0): sample on rising. The counter advances on
//   the falling edge, so this samples mid-window. Mode 1 would sample on the
//   counter transition itself. Verified on hardware.
//   CPOL=0 idles SCLK low, and that idle IS the frame reset.
//   No CS (spics_io_num = -1), no MOSI.

// Per frame — the gap since the previous transaction must be >= 5*tau:
spi_device_transmit(dev, &{ .rxlength = 16 * num_modules, .rx_buffer = rx });

for (ch = 0; ch < total_channels; ++ch)
    raw[ch] = ((rx[ch / 8] >> (7 - (ch % 8))) & 1) ^ active_low;
```

16·N bits is always a whole number of bytes (2·N), and MSB-first ordering puts
sample 1 — line 0 of module 1 — in the top bit of `rx[0]`, so the unpack is a
shift rather than a lookup.

Three properties follow from using DMA rather than a CPU loop, and the first is
a correctness requirement, not an optimization:

- **The burst is gapless.** The chain holds its bus grant on a capacitor that
  releases after ~6 µs without a clock edge, so a mid-frame stall silently
  resets every counter in the chain and produces a plausible-looking frame whose
  tail is re-read from module 1. A bit-banged loop on FreeRTOS cannot guarantee
  6 µs.
- **No pacing timer.** The transaction cadence *is* Fs (FR-SCAN-8). Jitter in
  *starting* a transaction only lengthens the idle gap, which is harmless.
- **The CPU cost is just the unpack plus the integrator** — 6.4% of one core at
  128 channels, against 23.5% for the bit-banged design this replaces.

The one hard constraint the driver must enforce is the reset gap: `1/Fs −
16·N/f_spi ≥ 5τ`. If it does not hold, the chain never clears between frames,
so `init()` refuses to start rather than clamping (FR-SCAN-9, `TIMING.md` §2.6).
At the default 2 MHz and 10 kHz the gap runs from 92 µs at one module to 36 µs
at eight, against a 28 µs floor.

The inter-module handoff needs no settle handling (FR-SCAN-10): the high-Z
window falls between sample slots by construction, so the old two-tier
in-module/boundary settle model is gone.

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
- **NVS** (`machine_config`): runtime machine profiles — channel map, colors,
  integrator params, profiler locks (FR-CFG-1). Boots to Kconfig defaults with
  no stored profile (FR-CFG-4). JSON import/export is `future`.

With 1:1 LED mapping and a single sense bus, the whole topology stays
expressible in Kconfig, so NVS profiles remain an M3 item rather than a
prerequisite.

Pin map (QT Py ESP32-S3). **The POC defaults are unusable on this board**:
GPIO 26/27 are SPI flash pins on the S3, and 15/25 are not broken out.

| Signal | GPIO | Board name |
|---|---|---|
| `CLK` (SPI `SCLK`) | 18 | A0 |
| `DATA` (SPI `MISO`) | 9 | A2 |
| LED string | 8 | A3 |
| Status pixel | 39 (power enable 38, drive high) | onboard NeoPixel |
| Profiler re-arm | 0 | BOOT button |

GPIO 17 (`A1`) is **free** — it was `/MR`, and the chain has no reset
conductor. The sense bus now costs two pins.

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
   `mr_pin` and the bit-bang loop, one receive-only mode-0 transaction per
   frame, unpack 16·N bits, and enforce the reset gap `1/Fs − 16·N/f_spi ≥ 5τ`
   at init. Add the boot feasibility check that feeds the *measured* Fs to
   `Filament::init()` and `Profiler::init()`. Batch the LED frame into one
   transmit (`lamp_map::render()` currently issues one strip transmit per
   channel, which is FR-LED-6's exact anti-pattern). Needs no chained hardware —
   a single module exercises the whole path.
4. **M1b — one module, live.** 16 channels off real lamp taps driving LEDs.
   Scope the bus at a module boundary to validate the 200 ns settle and confirm
   the bias resistor is fitted pull-down. Validate against a steady lamp and a
   matrixed lamp. *(partially done — the sense path is proven end to end on a
   breadboard module: counter counts, address decode is correct, the mux drives
   `DATA_IN`, and test inputs read at the right channel indices with correct
   polarity. See `BRINGUP.md`. The remainder is '251-dependent: `Q3` bank
   select, the 1 kΩ bias, real lamp taps, and the settle measurement.)*
5. **M1c — full chain.** 8 modules / 128 channels. The chain-specific risks all
   live here: accumulated clock skew at module 8 (`TIMING.md` §4.3), `ACT` fall
   time against the 5τ gap, the handoff window landing between sample slots, and
   whether Fs holds at 10 kHz with only 36 µs of idle margin. Compare measured
   frame period at 1, 4, and 8 modules against §2.4.
6. **M2 — profiler.** Time-based observation window, inter-edge histogram for
   period estimation, AC_DIMMED vs AC_STEADY, ground-bounce robustness. Boot
   classification seeds integrator params; verify a matrixed lamp reads as
   steady-on and a dimmed GI tracks brightness. BOOT button re-arms.
7. **M3 — profiles.** NVS profile load/save; per-channel tint and profiler
   locks; power cap; first named-machine calibration profile.
8. **M4 — polish.** Gamma/dither tuning, docs, release BOM.

## 6. Test strategy

- **Host unit tests** for `filament` (step response → verify tau) and the
  profiler classifier (synthetic waveforms → expected class). Pure fixed-point
  math, no hardware needed.
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
- The POC's single-'251/8-channel path remains a valid `channels_per_module=8,
  num_modules=1` configuration.
- The POC's `active_low=true` default is **superseded**. The v2 front end is
  MOSFET (inverting) → Schmitt inverter (inverting) = non-inverting overall, so
  lamp on is logic high (HW-1). The bus bias resistor must be oriented to match
  (HW-2): floating bus reads *off*, which with a non-inverting front end means a
  pull-down.
