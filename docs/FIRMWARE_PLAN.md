# Firmware Plan — `pinled` v2 (ESP-IDF 5.5.x, ESP32-S3)

First-cut architecture for the v2 firmware that replaces the single-mux POC.
Pairs with `REQUIREMENTS.md` (req IDs referenced inline), `TIMING.md` (scan/LED
budgets and bus electrical), and `DOSSIER.md` (design rationale).

Target board: **Adafruit QT Py ESP32-S3**. The S3 matters: `dedic_gpio` gives
~8 ns GPIO access where ESP32 classic tops out at ~25 ns of direct register
access, which is what makes a single fixed sample rate viable across the whole
8..128 channel range.

## 1. Module map

```
main/                         app entry, task wiring, config glue (ooe::pinled::Main)
components/
  lamp_scan/                  74HC161 + dual 74HC251 scan driver  → raw per-channel samples
  filament/                   per-channel leaky integrator        → brightness 0..255
  profiler/                   drive-scheme classifier             → per-channel {class,params}
  lamp_map/                   channel → LED index/color + WS2812B (RMT) render
  machine_config/            NVS-backed profiles + Kconfig defaults
```

Dependency direction (no cycles):

```
main ─▶ machine_config
main ─▶ lamp_scan ─▶ (gpio)
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

- **`scan_task`** (pinned to core 1, paced by `gptimer` → task notification):
  one frame per tick — `reset → for each channel: read → integrator.update(ch,
  sample) → clock`. Fixed **10 kHz/channel regardless of channel count**
  (FR-SCAN-5/8); at the 128-channel maximum a frame is ~23.5 µs, or 23.5% of
  core 1. Not free-running: a fixed rate is what makes the filament time
  constants mean the same thing on an 8-channel bench rig and a 128-channel
  install, and uniform `dt` is what the integrator assumes. The `dedic_gpio`
  bundle is created *inside this task*, because on the S3 a bundle is bound to
  the core that created it. This is where the POC's `count()` / `check_state()`
  live, generalized to the chained-module bus.
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

Generalizes the POC to the chained bus. Config:
`{clk_pin, mr_pin, data_pin, num_modules, channels_per_module, active_low}`.

The counters cascade via TC → ENT/ENP, so the chain is one wide binary counter:
bits 0–2 select the mux channel, bit 3 bank-selects the two '251s in a module,
bits 4+ are the module index. Each module decodes its own index and puts its
'251s in high-Z when unaddressed, so **all modules share one `DATA_IN`** —
a single serial walk, not a parallel read per module (FR-SCAN-3):

```
mr_pulse();                        // async clear → whole chain to count 0
settle(BOUNDARY);                  // module 0 /OE turning on into a floating bus
for (ch = 0; ch < total_channels; ++ch){
    raw[ch] = read(data_pin) ^ active_low;
    clk_pulse();                   // advance the whole chain one count
    settle(is_boundary(ch) ? BOUNDARY : IN_MODULE);
}
```

The cascade is synchronous, so the module-index bits are valid in one CLK→Q, not
N of them; TC propagation only caps clock frequency at ~18 MHz, three orders of
magnitude above where we run. Because `/MR` clears the chain every frame, the
counter never has to wrap to self-align and **any module count 1..8 is legal**,
including non-power-of-two counts like 6 modules / 96 channels.

Settle is two-tier (FR-SCAN-10): ~100 ns within a module, ~200 ns at a module
boundary where a `/OE` handoff has to recharge the ~150 pF bus through the 1 kΩ
bias. See `TIMING.md` §2.3–2.4.

Frame time is linear in total channel count — 3.2 µs at 16 channels, 23.5 µs at
128 — which is exactly why the sample rate is paced and boot-validated rather
than assumed (FR-SCAN-8/9) instead of taken from a Kconfig constant.

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
| `CLK` | 18 | A0 |
| `/MR` | 17 | A1 |
| `DATA_IN` | 9 | A2 |
| LED string | 8 | A3 |
| Status pixel | 39 (power enable 38, drive high) | onboard NeoPixel |
| Profiler re-arm | 0 | BOOT button |

This leaves I2C/STEMMA QT (6/7, 40/41), SPI (35/36/37), and the UART (5/16)
free.

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
   builds on S3; light the onboard status pixel.
3. **M1a — real time base.** `lamp_scan` rewritten for the shared serial bus
   (single `DATA_IN`, two-tier settle), `dedic_gpio` bundle created inside the
   pinned scan task, `gptimer` pacing at 10 kHz, and the boot feasibility check
   that measures frame time and feeds the *measured* Fs to `Filament::init()`
   and `Profiler::init()`. Batch the LED frame into one transmit.
4. **M1b — one module, live.** 16 channels off real lamp taps driving LEDs.
   Scope the bus at a module boundary to validate the 200 ns settle and confirm
   the bias resistor is fitted pull-down. Validate against a steady lamp and a
   matrixed lamp.
5. **M1c — full chain.** 8 modules / 128 channels. Confirm settle holds at the
   far end of 800 mm and that Fs stays at 10 kHz. Compare measured frame time at
   1, 4, and 8 modules against the `TIMING.md` §2.4 model.
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
