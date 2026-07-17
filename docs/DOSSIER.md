# Project Dossier — Pinball Light Strip (`pinled` v2)

**Project home:** https://oneoffendeavors.com/projects/001-pinball-light-strip/
**Firmware repo:** https://github.com/jeffersonjhunt/pinled (this document ships in `docs/`)
**Status:** in-progress (design consolidation / v2 firmware bring-up)
**Author:** Jefferson J. Hunt · One Off Endeavors (`ooe`)
**License:** MIT (firmware) · hardware to be published as schematics + gerbers + BOM

---

## 1. What this is

A universal, drop-in LED replacement for the incandescent lamps in vintage
pinball machines. Instead of splicing into the machine's logic, the strip
**senses the existing lamp drive** at (or near) each socket, reconstructs how
bright the original bulb *would* have been, and reproduces that on an
addressable LED (WS2812B / SK6812). Because the sensing and mapping live in
firmware, one board adapts to Bally, Williams, Stern, Gottlieb, and EM games
via configuration rather than a new PCB per machine.

Design targets from the project page: **$15–25 per strip**, **5–12 V** common
pinball rails with onboard regulation, KiCad + PlatformIO/ESP-IDF toolchain,
and open-source schematics/gerbers/firmware/BOM.

### Why not just drive LEDs directly?

The whole point is *non-invasive retrofit*. The machine already decides which
lamps are on, dims them, flashes them, and runs its light shows. If we sense
the original drive we inherit all of that behavior for free and stay compatible
with every game the original ROM knows how to run — no rewiring, no protocol
reverse-engineering, no per-title lamp tables required to get baseline
behavior.

---

## 2. The core idea: model the filament

The single insight that makes "universal" tractable:

> An incandescent bulb is a thermal low-pass filter. Its brightness is the
> *time-average* of the power delivered to it, with a time constant of roughly
> **20–50 ms**. Every pinball lamp-drive scheme was designed to look correct
> *through that filter*.

So the firmware's job is to **be that filter**. A per-channel leaky integrator
tuned to the filament time constant does the physically correct thing for every
era automatically:

| Original drive scheme | Raw electrical signal | Through the filament model → LED |
|---|---|---|
| EM steady DC/AC | constant on | integrator saturates → full brightness |
| Solid-state lamp **matrix** (strobed, ~1 kHz, ~1/8 duty) | sub-ms pulse train | smoothed to a steady glow, exactly like the bulb |
| Dimmed GI (zero-cross / triac phase control) | phase-chopped AC | tracks conduction angle → proportional brightness |
| Lamp effects (flash/chase, tens–hundreds of ms) | slow on/off | **passes through** → LED flickers like the bulb did |

The magic is that the integrator's time constant sits in exactly the gap
between the *matrix strobe period* (a few ms — must be smoothed away) and the
*fastest intended visible effect* (tens of ms — must pass through). That gap is
where the real filament lives, which is why emulating it is the right answer
and not a compromise.

**Hardware corollary:** keep the analog front end *fast and dumb*. Do **not**
RC-filter each input to steady DC in hardware — a fixed RC bakes in one time
constant and destroys universality. Threshold to a clean digital bit, sample it
fast, and let the *duty cycle* of that digital pulse train carry the brightness.
The time constant lives in firmware, where it is reprogrammable per machine.

---

## 3. Signal-chain architecture

```
                 ┌─────────────────────────── one 16-channel module ───────────────────────────┐
per-socket taps  │                                                                              │
 (5–20 V, AC/DC) │   FET level-shift + protection      74HC251 #A (ch 0–7, tri-state Y)         │
   L0 ───────────┼──▶ [divider · diode · clamp] ──────▶ D0..D7 ─┐                               │
   ...           │                                              ├─▶ Y (bussed) ──▶ ESP32 GPIO   │
   L15 ──────────┼──▶ [divider · diode · clamp] ──────▶ D0..D7 ─┘        (DATA_IN, shared)      │
                 │   74HC251 #B (ch 8–15, tri-state Y)                                          │
                 │                                                                              │
                 │   74HC161 counter:  Q0..Q2 ─▶ select A/B/C on BOTH '251s                     │
                 │                     Q3     ─▶ /OE of '251#A  and (via inverter) /OE of '251#B │
                 │   CLK  ◀── ESP32 GPIO (shared bus)                                            │
                 │   /MR  ◀── ESP32 GPIO (shared bus)                                            │
                 └──────────────────────────────────────────────────────────────────────────────┘

ESP32 ── 1 GPIO ──▶ WS2812B / SK6812 string (RMT)   ← firmware maps sensed channel → LED index(es)
```

**Per module the ESP32 spends 3 GPIO:** `CLK`, `/MR` (reset), `DATA_IN`.
Clock and reset are a shared bus across all modules; each module contributes one
`DATA_IN` line. So *N* modules = `2 + N` input-side pins, plus **one** GPIO for
the entire LED string.

### Why the '251 and not the '151

This is the crux of the earlier open-drain/open-collector discussion. The
`74HC151` has **push-pull** outputs: when you disable it (strobe high) it does
**not** go high-impedance — it actively drives `Y` **low**. Two '151s cannot
share a data line; the disabled one fights the active one → bus contention.

The **`74HC251` is the tri-state-output version** of the '151. Disabled, its `Y`
goes to true high-Z, so two '251s can be wired onto one `DATA_IN` line and
bank-selected by the counter's `Q3`. That is what lets a 16-channel module read
out on a single ESP32 input pin. (The POC used a single '151 for 8 channels on
one pin, which is fine — the contention problem only appears when you bus two.)

**Counter note:** the `74HC161` is a synchronous 4-bit binary counter with an
**asynchronous** active-low clear (`/MR`). A low pulse on `/MR` zeroes
`Q0..Q3` immediately without a clock edge, so a scan frame is: assert `/MR`,
then read-then-clock through counts 0..15. (Its cousin the `74HC163` has a
*synchronous* clear; don't substitute without changing the reset timing.)

### Daisy-chaining / expansion (16 channels per module)

Recommended **star** topology for reliability: every module shares the `CLK` and
`/MR` bus so all counters step in lockstep and present the same channel index at
the same time; each module returns its bit on a **dedicated** `DATA_IN` GPIO.
Simple, no cross-module contention, and the ESP32 reads all modules in the same
scan loop. A **bussed** alternative (all module data lines tri-stated onto one
pin with extra address bits from a cascaded second '161) saves pins at the cost
of more glue logic and is documented as a future option, not the default.

---

## 4. Auto-profiling

Because the front end preserves the raw pulse pattern, firmware can **classify
each channel** by watching its digital transitions over a short observation
window and pick the right integrator gain/calibration automatically, instead of
shipping a hand-tuned table per game:

| Class | Signature the profiler looks for | Handling |
|---|---|---|
| `STEADY` | ~100% duty, no edges (EM DC, always-on GI) | unity gain, full brightness when present |
| `MATRIX` | periodic bursts ~a few hundred Hz–1 kHz, low duty (~1/8) | normalize duty → full brightness; short attack to catch column strobe |
| `AC_STEADY` | 100/120 Hz envelope, ~50% raw duty | envelope-follow, on/off |
| `AC_DIMMED` | 100/120 Hz with variable conduction angle | map conduction angle → brightness (triac/zero-cross dimming) |
| `OFF` / `ABSENT` | no activity across window | LED off; skip / low-power |

The classifier runs at boot (and can be re-armed on demand). Its output per
channel is `{class, duty_norm, period_est, confidence}`, which seeds the
filament integrator's gain and attack/decay. Machine **profiles** (NVS-stored)
can override or lock any channel when a game needs it, so auto-detect handles
the common case and config handles the oddballs.

---

## 5. Sampling budget

To capture duty faithfully, sample each channel well above the highest strobe /
chop frequency — target **a few kHz per channel**. For a 16-channel module that
is ~16–32 kHz of mux stepping, trivial for the ESP32 whether bit-banged (as the
POC does) or clocked from a timer/`dedic_gpio`. Sequential-scan phase skew
washes out because each channel is integrated over a window spanning many matrix
frames and AC cycles.

- **Matrix strobe** to smooth away: ~1 ms period → sample ≥ several kHz.
- **Zero-cross AC**: 100/120 Hz → resolve conduction angle with fine sampling
  across the half-cycle.
- **Filament integrator window**: ~20–50 ms (config per machine).
- **LED refresh**: 60–120 Hz frame rate to the WS2812B string is plenty; the
  integrator decouples sample rate from render rate.

> ⚠️ **Aliasing caution:** a flat *full-frame* rate near the matrix strobe rate
> (e.g. sampling each channel only ~60×/s while the matrix strobes ~1 kHz)
> produces beat frequencies that show up as slow LED pulsing/dropout. Oversample
> per channel; render slower.

---

## 6. Bill of materials (per 16-channel module — indicative)

| Ref | Part | Qty | Notes |
|---|---|---:|---|
| U1 | 74HC161 | 1 | 4-bit sync binary counter, async clear. Q0–Q2 = mux select, Q3 = bank select. |
| U2, U3 | 74HC251 | 2 | 8:1 mux, **tri-state** Y output. Bussed `DATA_IN`. |
| U4 | 74HC04 (or spare gate) | 1 | inverter for Q3 → second '251 `/OE`. |
| Q1–Q16 | N-ch MOSFET (e.g. 2N7002 / BSS138) | 16 | per-channel level-shift 5–20 V → 3.3 V logic (inverting common-source). |
| D1–D16 | signal diode (e.g. 1N4148 / BAT54 Schottky) | 16 | AC-signal rectification / input steering. |
| — | gate series R + pull, drain pull-up, clamp to 3V3 | — | protection + defined trip point with hysteresis. |
| U5 | ESP32 module | 1 | POC: Adafruit QT Py ESP32 Pico. Original ESP32 target. |
| — | WS2812B / SK6812 | n | addressable LEDs, one data line for the string. |
| — | 3V3 regulator + decoupling | 1 | onboard reg from 5–12 V rail; 0.1 µF per IC. |

> LED power budget is separate from logic — size the 5 V LED supply for the
> string, not off the sense logic rail.

### Front-end (per channel) design notes
- **Inversion:** a common-source MOSFET outputs **low** when the input is
  **high**. Decide polarity deliberately and invert in firmware
  (`LAMPSCAN_ACTIVE_LOW`).
- **Voltage span:** design the divider/clamp for the worst case across eras
  (~6.3 V GI up to ~18–20 V feature-lamp drive), AC or DC, either polarity, with
  hysteresis so marginal signals don't chatter.
- **Protection:** series gate resistors + clamp the FET output to the 3V3 rail
  (Schottky or the input's own protection diodes via series R, or a TVS). The
  machine is electrically noisy — the machine's own filtering helps, but don't
  rely on it exceeding the HC absolute-max of VCC + 0.5 V.

---

## 7. Assumptions & open questions

Stated so the first cut can proceed; flag any to change:

1. **Topology:** dual `74HC251` shared-line, 16 ch/module, Q3 bank-select
   (this is the "261" from the brief). *Assumed.*
2. **MCU:** original ESP32 (QT Py ESP32 Pico), ESP-IDF **5.5.x**, matching the
   POC's `CMakePresets`. S3 is a drop-in later (more RAM/RMT channels).
3. **LED driver:** `zorxx/neopixel` (RMT) as in the POC; `espressif/led_strip`
   is a documented alternative.
4. **Sensing point:** at/near each **socket** (one tap = one lamp = one LED),
   so no matrix row/column decoding is required.
5. **First target machines:** not yet fixed. The profiler + config layer is
   generic; real per-machine calibration profiles want a named first target
   (era, GI scheme, lamp voltages, matrix rate) to validate against.

**To move forward I'd want:** the first one or two target machines (manufacturer
+ era), so the profiler thresholds and the first shipped config profile are
validated against a real drive scheme rather than nominal datasheet numbers.
