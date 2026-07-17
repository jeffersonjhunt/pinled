# Firmware Plan — `pinled` v2 (ESP-IDF 5.5.x)

First-cut architecture for the v2 firmware that replaces the single-mux POC.
Pairs with `REQUIREMENTS.md` (req IDs referenced inline) and `DOSSIER.md`
(design rationale).

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

- **`scan_task`** (high rate, pinned to a core): tight loop —
  `reset → for each channel: read → integrator.update(ch, sample) → clock`.
  Target ≥ 2 kHz/channel (FR-SCAN-5). This is where the POC's `count()` /
  `check_state()` live, generalized to 16-ch modules.
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

Generalizes the POC. Config: `{clk_pin, mr_pin, data_pins[num_modules], channels_per_module, active_low}`.
Frame:

```
mr_pulse();                        // async clear → count 0
for (ch = 0; ch < channels; ++ch){
    for (m = 0; m < num_modules; ++m)
        raw[m][ch] = read(data_pins[m]) ^ active_low;
    clk_pulse();                   // advance all counters in lockstep
}
```

Q3 bank-select on each '251 is automatic from the counter — no extra ESP pin.
Interface returns booleans per (module, channel); the integrator consumes them.
`lamp_scan` is written for MVP; a timer/`dedic_gpio` backend is a later drop-in
(FR-SCAN-7) behind the same API.

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

### 3.4 Mapping + render (`lamp_map`)

`channel[g] → { led_index, base_rgb, tint, gamma }`. Render multiplies base
color by the channel's brightness, gamma-corrects, optionally dithers low levels
(FR-LED-5), and writes the WS2812B frame. Uses `zorxx/neopixel` (RMT) as the POC
does; `espressif/led_strip` is a swap-in alternative.

## 4. Configuration

- **Kconfig** (`main/Kconfig.projbuild`): pins, channel count, modules, default
  time constants, sample/refresh rates, LED count, active-low — so
  `idf.py menuconfig` sets sane build-time defaults (FR-CFG-2).
- **NVS** (`machine_config`): runtime machine profiles — channel map, colors,
  integrator params, profiler locks (FR-CFG-1). Boots to Kconfig defaults with
  no stored profile (FR-CFG-4). JSON import/export is `future`.

## 5. Bring-up milestones

1. **M0 — skeleton compiles.** Components register, `Main` boots, version logs,
   LED string shows a heartbeat. *(this cut)*
2. **M1 — scan + integrate 16 ch.** `lamp_scan` reads a real dual-'251 module;
   filament integrator drives LEDs from live lamp taps. Validate against a
   steady lamp and a matrixed lamp.
3. **M2 — profiler.** Boot classification seeds integrator params; verify a
   matrixed lamp reads as steady-on and a dimmed GI tracks brightness.
4. **M3 — multi-module + profiles.** N modules on shared bus; NVS profile load;
   first named-machine calibration profile.
5. **M4 — polish.** Gamma/dither, color/tint config, docs, release BOM.

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
  NEOPIXEL=GPIO15) are carried as Kconfig defaults.
- The POC's single-'151/8-channel path remains a valid `channels_per_module=8,
  num_muxes=1` configuration.
