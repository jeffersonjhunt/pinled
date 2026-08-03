# Timing & Electrical Budget — `pinled` v2

Sizing for the chained-module architecture: how long a scan frame takes, how
long an LED frame takes, and what the shared sense bus and the power
distribution have to do for those numbers to hold. Pairs with
`REQUIREMENTS.md` (req IDs inline) and `FIRMWARE_PLAN.md`.

Target hardware: **Adafruit QT Py ESP32-S3** (dual LX7 @ 240 MHz).

> Numbers marked *(bench)* are estimates from datasheet typicals and need a
> scope to confirm. Everything downstream of them is arithmetic.

## 1. The configuration range

The system scales by chaining 16-channel modules on one bus, so almost every
budget below is linear in module count. That range is the whole problem:

| | Min | Max |
|---|---|---|
| Modules | 1 | 8 |
| Channels (16/module) | 8 (single '251) | 128 |
| LEDs (1:1 with channels) | 8 | 128 |
| Harness length (100 mm hops) | 100 mm | 800 mm |

A single fixed `sample_rate_hz` constant cannot be correct across a 16× spread
in frame time. §2.5 is how that gets resolved.

## 2. Sense side

### 2.1 Topology

Modules are **identical and unaddressed**. Each holds the forwarded clock until
it has scanned its own 16 lines, then becomes transparent:

```
CLK_OUT = CLK_IN AND DONE
```

so the clock itself is the token and the master simply issues 16·N pulses. All
modules share one `DATA` bus; exactly one 3-state mux drives it at any instant,
arbitrated by an RC activity detector plus the DONE latch. Full protocol in
`CHAINING.md`.

There is no `/MR` conductor. The frame reset is a **clock-idle timeout**: hold
`CLK` low for ≥ 5τ and every activity detector discharges, async-clearing all
counters and DONE latches. Any module count 1–8 is legal, including
non-power-of-two configurations like 6 modules (96 channels).

Two consequences the old cascaded-counter topology did not have:

- **Clock skew is cumulative.** Each module inserts a NAND plus a Schmitt
  inverter, so module *k*'s clock lags the master's by *k·d*. Module *k*'s data
  window for bit *n* is shifted later by the same amount, so the master's sample
  point stays inside it as long as `k·d < T_clk`. With LVC (~10–13 ns/module)
  that is ~100 ns across 8 modules against a 500 ns bit — 5× margin. With HC at
  3.3 V it is ~0.5 µs and the far end fails, *as a function of chain length*
  (HW-3).
- **The chain has a stall tolerance.** ACT decays with τ; if clocking stops
  mid-frame for longer than ~6 µs the whole chain resets. See §2.5.

### 2.2 Why SPI + DMA, not a CPU loop (FR-SCAN-7)

The chain holds its bus grant on a capacitor. `ACT` peaks at ~2.7 V (3.3 V less
a diode drop) and an LVC Schmitt releases at `V_T-` ≈ 0.9 V, so with τ = 5.6 µs:

```
t_stall = τ · ln(2.7 / 0.9) ≈ 6.2 µs
```

**A gap in clocking longer than ~6 µs mid-frame silently resets every counter
and DONE latch in the chain.** On an S3 running FreeRTOS, a 6 µs stall from a
cache miss on a flash read or a higher-priority ISR is routine, so a bit-banged
loop — however fast its individual GPIO operations — is the wrong tool. The
failure is also nasty: not a dropped frame, but a *plausible-looking* frame
where the tail is re-read from module 1.

SPI removes the question. A DMA transaction is gapless by construction:

| SPI | Chain |
|---|---|
| `SCLK` | `CLK` |
| `MISO` | `DATA` |
| Mode | 0 (`CPOL=0, CPHA=0`) |

Mode 0 samples on the rising edge. Because the counter advances on the
**falling** edge, that puts the sample in the middle of each line's data window
rather than on its boundary — mode 1 would sample exactly on the counter
transition. `CPOL=0` idles `SCLK` low between transactions, which *is* the frame
reset. 16·N bits is always a whole number of bytes.

This was measured, not assumed: on a rig clocking the '161 directly, both modes
sampled after the counter advanced and shifted every channel by one. The
Schmitt inversion in a real module is what creates the stable window.

This deletes work rather than adding it: no `dedic_gpio` bundle to pin to a
core, and no separate pacing timer, because the transaction cadence sets Fs
directly (§2.5).

**Gotcha:** the filament math below assumes the LX7 runs at **240 MHz**, and
ESP-IDF's default for `esp32s3` is **160 MHz**, which inflates per-channel
integrator cost by 1.5×. `sdkconfig.defaults` must set
`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` explicitly (NFR-7). This was not set on
the first S3 bring-up build and the boot log read `cpu freq: 160000000 Hz`;
confirm it there rather than assuming it.

### 2.3 Settle and the handoff

At 2 MHz a bit slot is 500 ns. Contributors inside one slot:

| Contributor | Typical (LVC @ 3.3 V) |
|---|---|
| '161 CLK→Q | ~8 ns |
| '251 address → output | ~10 ns |
| Bus slew, ~150 pF | ~20 ns |
| Accumulated chain skew, 8 modules | ~100 ns |
| **Total worst case** | **~140 ns** |

Comfortably inside 500 ns. The **module handoff needs no settle budget of its
own**: module *k* releases `DATA` on the falling edge that wraps its counter and
module *k+1* arms a half-period later, so the high-Z window sits *between*
sample slots and is never sampled (FR-SCAN-10). This is why the old two-tier
in-module/boundary settle model is gone.

Frame start is the one place worth measuring: `ACT` is charging during the first
half-period, so line 0 of module 1 is the tightest sample in the frame. Charge τ
is ~12 ns against a 250 ns half-period, so it should be clean — but confirm on
the bench, and clock one dummy bit first if it is not.

### 2.4 Frame budget

The scan is now pure hardware: 16·N SPI bits at 2 MHz, DMA-fed, zero CPU. The
only CPU cost is the filament integrator (~50 ns/channel) plus per-transaction
setup.

| Ch | Mod | Bits | Scan @ 2 MHz | Idle (≥5τ) | Min frame | Max rate | CPU @ 10 kHz |
|---|---|---|---|---|---|---|---|
| 16 | 1 | 16 | 8 µs | 28 µs | 36 µs | 27 kHz | 0.8% |
| 32 | 2 | 32 | 16 µs | 28 µs | 44 µs | 23 kHz | 1.6% |
| 64 | 4 | 64 | 32 µs | 28 µs | 60 µs | 17 kHz | 3.2% |
| 96 | 6 | 96 | 48 µs | 28 µs | 76 µs | 13 kHz | 4.8% |
| 128 | 8 | 128 | 64 µs | 28 µs | 92 µs | **10.9 kHz** | **6.4%** |

Two things changed versus the bit-banged model this table replaces. The 10 kHz
target now *just* fits at 128 channels rather than sitting at 43 kHz of
headroom — the binding constraint is the 5τ reset gap, not the scan. And CPU
occupancy dropped from 23.5% to 6.4%, because the only work left on the core is
the integrator.

At a fixed 10 kHz (100 µs period) every configuration leaves an idle gap well
above 5τ: 92 µs at one module, 36 µs at eight. The margin narrows as modules are
added, which is exactly what FR-SCAN-9's boot check exists to catch.

> Scaling levers, in order of preference, if 128 channels at 10 kHz proves
> tight: raise the SPI clock (skew allows ~3 MHz with LVC), or shrink τ (which
> costs stall tolerance).

### 2.5 Paced sampling (FR-SCAN-8)

**Run at one fixed rate regardless of channel count. Default 10 kHz.**

Rationale:

- Feasible at 128 channels (6.4% of one core), so behavior is identical on a
  16-channel bench rig and a 128-channel install. The filament time constants
  mean the same thing in both.
- 10 kHz gives ~20 samples across a 2 ms matrix strobe pulse, and 833 samples
  per 60 Hz half-cycle — conduction-angle resolution ~0.1%. Comfortable for both
  the duty estimate and the profiler.
- Uniform `dt` is what the leaky integrator assumes. A burst-then-sleep pacing
  scheme would reintroduce exactly the aliasing against the matrix strobe that
  the filament model exists to kill.

Mechanism: the SPI transaction cadence *is* the pacing. Queue one receive-only
transaction per frame at Fs; the hardware clocks 16·N bits gaplessly and the
gap between transactions supplies the ≥5τ reset. Jitter in *starting* a
transaction is harmless — it lengthens the idle gap, which the chain does not
care about, unlike jitter *within* a frame, which the chain cannot survive at
all.

The one hard floor: the inter-transaction gap must never fall below 5τ (28 µs).
At 10 kHz with 8 modules the gap is 36 µs, which is the tightest configuration
in the range. FR-SCAN-9's boot check exists to verify this rather than assume
it.

### 2.6 Boot feasibility check

Config is not trusted. At startup:

1. Compute `t_scan = 16·N / f_spi` and check `1/Fs − t_scan ≥ 5τ`. **Refuse to
   start** if the configured rate leaves less than the reset gap — that is not a
   degraded frame, it is a chain that never resets.
2. Run ~256 frames and measure actual frame period, which also catches a
   mis-specified `f_spi` or a driver that silently rounds the clock divider.
3. Clamp the configured rate so both the 5τ gap and a 60% CPU ceiling hold; log
   loudly if clamped.
4. Pass the **resulting** rate to `Filament::init()` and `Profiler::init()`, so
   the tau→coefficient math is derived from what the hardware actually does.

Step 1 is new and is the important one: with the old `/MR` topology an
over-ambitious rate merely ran late, but here it eats the reset gap and the
chain stops clearing between frames.

### 2.7 Consequence for the profiler

The observation window must be defined **in milliseconds, not frames**. The POC
observes a fixed 512 frames; at 10 kHz that is 51 ms — roughly three AC cycles,
far too short to classify AC drive. Budget 500 ms–1 s and derive the frame count
from the measured Fs.

## 3. LED side

WS2812B/SK6812: 24 bits × 1.25 µs = **30 µs/LED**, plus reset (≥50 µs original
WS2812B, ≥80 µs SK6812, **280 µs on WS2812B-V5** — budget 300 µs).

```
t_led(N) = N × 30 µs + 300 µs
```

| LEDs | Frame | @60 Hz | @90 Hz | @120 Hz |
|---|---|---|---|---|
| 32 | 1.26 ms | 8% | 11% | 15% |
| 64 | 2.22 ms | 13% | 20% | 27% |
| 96 | 3.18 ms | 19% | 29% | 38% |
| 128 | 4.14 ms | 25% | 37% | **50%** |

At 1:1 channel-to-LED mapping the maximum configuration fits comfortably on a
**single chain, single RMT channel, at 120 Hz**. Multi-chain output, parallel
RMT transmit, and the `zorxx/neopixel` → IDF RMT driver swap are all
unnecessary — they were only required for multi-LED-per-lamp groups.

What does remain:

- **Batch the frame.** One `neopixel_SetPixel()` call with the full pixel array
  per frame. The POC calls it once per channel inside the render loop, and each
  call pushes the entire strip — 128 full transmits per frame.
- **Power cap (FR-LED-7).** 128 LEDs at the warm-white base is ~6 A / 30 W worst
  case. A global brightness/current budget belongs in the render path, not in
  polish.
- **Level shift.** WS2812B wants V_IH ≥ 0.7 × VDD = 3.5 V; a 3.3 V S3 output is
  under spec. Standard fixes are a 74LVC1G17 buffer or running the strip at
  ~4.5 V.

## 4. Sense bus electrical

### 4.1 Load

Estimated at full extension (8 modules, 800 mm) *(bench)*:

| Contributor | Estimate |
|---|---|
| JST-SH harness, 800 mm | ~45 pF |
| 8 × '251 tri-state output | ~64 pF |
| Module traces | ~40 pF |
| S3 input | ~3 pF |
| **Total** | **~150 pF** |

With the 1 kΩ bus pull, τ ≈ 150 ns. That only governs how fast an *undriven*
bus decays, which matters at frame start and after the last module finishes —
never mid-frame, because the handoff window carries no sample.

### 4.2 Pull direction follows front-end polarity (HW-2)

The rule: **a floating or unpopulated bus must read *lamp off*.**

The front end is FET (inverting) → Schmitt inverter (inverting) = non-inverting
overall, so lamp on = logic high, and the pull is therefore **down**. This also
gives a safe pre-boot state: bus low, all lamps dark, before firmware runs.

If the front end were ever changed to inverting, the pull must flip to up and
`active_low` must flip with it. These two choices are one decision, not two.

There is a **third** thing bound to that same decision: the MCU's *internal*
pull on `DATA_IN`. `gpio_reset_pin()` leaves the internal pull-**up** enabled
and `gpio_set_direction()` does not clear it, so the naive setup contradicts the
rule above and an undriven bus reads *lamp on*. `lamp_scan::init()` therefore
configures the pin explicitly and derives the internal pull from `active_low`,
so all three flip together. The internal pull is ~45 kΩ and does not replace the
1 kΩ external bias — it only ensures the MCU is not fighting it.

### 4.3 Logic family (HW-3)

Two independent arguments land on the same answer, and the second is the one
that actually binds.

**Drive.** 1 kΩ against 3.3 V is 3.3 mA of standing load on whichever '251 is
driving:

| | 74HC251 @ 3.3 V | 74LVC251A @ 3.3 V |
|---|---|---|
| Output drive | ±2–4 mA (derated) | ±24 mA |
| t_pd | ~40–60 ns | ~5–8 ns |
| Slew 150 pF rail-to-rail | ~124 ns | ~20 ns |
| Holding against 1 kΩ | **marginal — may not reach valid V_OL/V_OH** | trivial |

**Clock skew — the binding constraint.** Every module inserts a NAND plus a
Schmitt inverter into the forwarded clock, so module *k* is clocked *k·d* late
and the master's sample point must still land inside its data window, i.e.
`k·d < T_clk`:

| Family | Per module | 8 modules | Verdict at 2 MHz (500 ns bit) |
|---|---|---|---|
| 74HC @ 3.3 V | ~50–70 ns | ~0.5 µs | **fails** — far end outside its slot |
| 74LVC @ 3.3 V | ~10–13 ns | ~100 ns | 5× margin |

The HC failure is chain-length dependent, which makes it the worst kind: a
4-module chain works and an 8-module chain does not, with nothing obviously
wrong in between. **Use LVC.**

Because LVC is not a 5 V part, the harness carries 5 V and every module
regulates 3.3 V locally (HW-8). `DATA` and `CLK` are therefore 3.3 V signals
throughout and the S3's lack of 5 V tolerance never becomes an issue.

### 4.4 Signal integrity

800 mm of harness at LVC edge rates settles reflections in ~25 ns, well inside
the settle budget. The real risk is CLK→DATA crosstalk with a single ground
return in a 5-conductor cable:

- **~100 Ω series termination on CLK at the source** to slow the edge and damp
  reflections.
- The frame order is already correct: `settle → read → clock` places the settle
  after the clock edge and samples DATA before the next transition.
- **Per-module decoupling is mandatory, not optional.** 800 mm of thin wire is
  ~0.5–0.8 µH of loop inductance; an LVC '251 slamming 150 pF cannot source that
  transient down the harness. Budget 100 nF per IC plus ~10 µF bulk per module.

## 5. Power & grounding

### 5.1 Distribution

Using a placeholder of **3 mA/channel** for the FET front end — *replace with
the measured value, it drives this whole section* — a 16-channel module is
~50 mA @ 3.3 V ≈ 175 mW, so eight modules ≈ **400 mA / 1.4 W**.

An LDO passes load current through, so raising the distribution voltage does not
reduce harness current; only a switching regulator does:

| Rail | Local reg | Harness current | Dissipation/module |
|---|---|---|---|
| 3.3 V | none | 400 mA | — |
| **5 V** | **LDO** | **400 mA** | **85 mW** |
| 9 V | LDO | 400 mA | 285 mW |
| 12 V | buck | ~135 mA | ~30 mW |

**5 V distributed, 3.3 V regulated locally** is the recommendation. 800 mm of
28–32 AWG is ~0.34–0.85 Ω round trip, so 400 mA drops 136–340 mV — on a 3.3 V
rail distributed directly that is 10% of the budget spent for nothing, whereas
at 5 V in it is absorbed entirely by the LDO's headroom. JST-SH is rated
1 A/contact, so 400 mA is comfortable either way.

9/12 V only pays off with a per-module buck, which puts eight unsynchronized
switchers next to an 800 mm harness carrying a 150 pF high-impedance bus. Hold
that in reserve for the case where measured front-end current is far above the
placeholder.

The connector's fifth pin should be labelled **`VIN`**, not `3V3`, so the same
harness survives a rail change.

### 5.2 Tapping the machine

At ~400 mA the thermals decide this:

| Tap | To 5 V via LDO | Verdict |
|---|---|---|
| Machine +5 V logic | not needed | **best** — filter and go |
| +12 V | 2.8 W | too hot for SOT-223 |
| Unregulated solenoid (~18–25 V, sags to ~12 V on coil fire) | ~6 W | no |

**An LDO is viable only off the machine's existing +5 V.** Anything higher needs
a buck — but that is *one* buck at the controller rather than eight at the
modules, and its output can be filtered before it enters the harness. Preferred
topology: +5 V tap (or one buck) at the head, local 3.3 V LDOs per module.

EM games may have no DC logic rail at all (6.3 VAC + ~25 VDC), so those need
rectify-and-buck.

Regardless of tap: **series Schottky + bulk electrolytic** at the input so the
board rides through rail sag when a coil fires, plus input clamping against
kickback. The filament integrator smooths a signal glitch; it does not smooth an
MCU brownout-reset, which blacks out all 128 lamps at once.

### 5.3 Grounding (HW-6)

Not a star — the harness is a daisy chain by construction, and that is fine.
What matters is narrower: **a single-point tie between pinled ground and the
machine's lamp-return ground**, made near the lamp matrix's return rather than
at the PSU. Powering from the machine makes that tie the power tap itself.

The harness ground drop (136–340 mV) lands on the **DATA line at the S3**, not
on the FET: the Schmitt and '251 are referenced to module-local ground, so the
offset appears as common-mode shift when the S3 samples. Against the S3's
~1.65 V of V_IL–V_IH margin that is up to 20% — acceptable, but a budget to
track, and another reason to keep harness current down.

The FET side is insensitive: 340 mV against a 5–20 V lamp swing is nothing.

### 5.4 Ground bounce and the profiler

A flipper or pop-bumper coil dumps tens of amps through the machine's ground
return, shifting every channel's effective threshold **simultaneously**. The
Schmitt trigger's hysteresis (V_hys ≈ 0.4–0.6 V for a 74LVC14 at 3.3 V) is the
primary defense and covers a lot of it.

Beyond that, the architecture is asymmetrically robust:

- **The filament model absorbs it.** A 1 ms false-on glitch against a 30 ms
  attack constant is a ~3% brightness bump, not a visible flash.
- **The profiler does not.** A coil firing during the observation window injects
  correlated false edges across every channel at once and will push steady lamps
  into the MATRIX bucket. Hence FR-PROF-5: use robust statistics, discard frames
  where an implausible number of channels transition together, and keep the
  re-arm path (BOOT button, GPIO 0) easy to reach.

## 6. Boot-time validation summary

Everything above collapses into a handful of checks the firmware runs and logs
at startup, so a mis-specified config fails loudly instead of quietly producing
wrong brightness:

| Check | Action on failure |
|---|---|
| `num_modules × channels_per_module ≤ 128` | refuse to start |
| `1/Fs − 16·N/f_spi ≥ 5τ` (reset gap fits) | **refuse to start** |
| measured `t_frame` vs configured Fs | clamp Fs, log, use the clamped value for tau |
| `t_led(led_count)` vs `refresh_hz` at 70% occupancy | clamp `refresh_hz`, log |
| `led_count ≥ mapped channel count` | log unmapped channels |

## 7. To confirm on the bench

1. **`ACT` fall time.** Scope `ACT` after the last clock edge; §2.2 predicts
   ~6.2 µs. This single number sets both the minimum reset gap and the chain's
   stall tolerance, so everything else depends on it.
2. **First bit at frame start.** `ACT` is charging during the first half-period,
   making line 0 of module 1 the tightest sample in the frame. If it is dirty,
   clock one dummy bit and discard.
3. **Accumulated clock skew.** Scope `CLK_OUT` at module 8 against `SCLK` at the
   master. §4.3 predicts ~100 ns with LVC; if it exceeds ~a third of a bit slot,
   drop the SPI clock.
4. **The handoff window.** Confirm the high-Z gap really does fall between
   sample slots and that no sample lands in it.
5. **Bus capacitance and pull direction.** Verify ~150 pF and that the 1 kΩ is
   fitted as a pull-**down**, at the master only.
6. **Front-end current per channel.** The 3 mA placeholder in §5.1 sets the
   entire power topology.
7. **CLK→DATA crosstalk** at full harness length, with and without series
   termination.
8. **Frame time vs. the model.** Compare measured frame period at 1, 4, and 8
   modules against §2.4.
9. **Coil-fire behavior.** Fire a flipper during a profiling window and confirm
   the classifier does not misbehave.
