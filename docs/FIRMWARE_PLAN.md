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
   a single module exercises the whole path. *(Done in code: single-transaction
   SPI path, 16·N unpack with no discards, `/PL` from `CS`, 10 kHz gptimer
   pacing, the boot feasibility clamp, and one strip transmit per frame. The
   **rev C** version of this was measured on the '161 + '151 rig at 1 MHz; the
   rev D framing that replaced it has never been run against hardware, so treat
   M1a as compiled-but-unproven until M1b passes. See `BRINGUP.md` §7.)*
4. **M1b — one '165 module on the bench.** Two '165s on a breadboard, 16 test
   inputs, driving LEDs. The three things to settle, in order:
   1. **SPI mode 2** — the only rev D claim that is reasoned rather than
      measured. Mode 2 should read the pressed channel; mode 3 its neighbour.
   2. **Bit order** — confirm channel 0 is the first bit out with channel 0 on
      input `H`, and that no byte-reverse is needed.
   3. **`/PL` from `CS`** — confirm the positive-polarity `CS` loads between
      frames, and that holding `/PL` low makes `DATA` track channel 0 live.
   *(Not started — blocked on parts. The firmware side is ready; the existing
   bench validation is all rev C hardware — '161 + '151 — and does not transfer
   beyond the SPI plumbing. See `BRINGUP.md` §5 for the rig.)*
5. **M1c — two modules, then eight.** Two modules is the important step: it
   proves the chain handoff, the 10 kΩ terminator, and that unplugging the last
   module blanks its channels to a stable zero rather than noise (HW-11). Then
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
- A single-'165/8-channel path remains a valid `channels_per_module=8,
  num_modules=1` configuration — useful for the first bench build.
- The POC's `active_low=true` default is **superseded**. The v2 front end is
  MOSFET (inverting) → Schmitt inverter (inverting) = non-inverting overall, so
  lamp on is logic high (HW-1). The bus bias resistor must be oriented to match
  (HW-2): an undriven net reads *off*, which with a non-inverting front end means a
  pull-down — one per receiving end since rev D.
