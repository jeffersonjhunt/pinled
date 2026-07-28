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

The '161 counters cascade via TC → ENT/ENP, so the whole chain behaves as one
wide binary counter: bits 0–2 select the mux channel, bit 3 bank-selects the
two '251s within a module, bits 4+ are the module index. Each module decodes
its own index and drops its '251s into high-Z when not addressed, so **all
modules share one `DATA_IN`**.

The cascade is synchronous, not ripple-through: every stage updates on the same
clock edge, so the module-index bits are valid in one CLK→Q, not N of them. TC
propagation only caps the maximum clock frequency:

```
t_TC(~30 ns) + t_setup,ENT(~15 ns) < t_CLK   →   f_max ≈ 18 MHz
```

We run three orders of magnitude below that. The cascade costs nothing.

A shared `/MR` (asynchronous clear on the '161) zeroes the entire chain at the
start of every frame, so **any module count 1–8 is legal** — the counter never
has to wrap to self-align, and non-power-of-two configurations like 6 modules
(96 channels) are fine.

### 2.2 Software cost per GPIO operation

| Method | Per op | Note |
|---|---|---|
| `gpio_set_level()` / `gpio_get_level()` | ~250–350 ns | what the POC uses |
| Direct register (`GPIO.out_w1ts`) | ~25–35 ns | 6–8 cycles |
| **`dedic_gpio`** | **~8 ns** | **2 cycles; S3 only** |

`dedic_gpio` does not exist on ESP32 classic — it is an S2/S3/C3+ peripheral.
Moving to the S3 is what promotes FR-SCAN-7 from *future* to baseline.

**Gotcha:** on the S3 the dedicated-GPIO instructions are per-core, and a bundle
is bound to whichever core created it. The bundle must be created **inside the
scan task after it is pinned**, not in `init()` from `app_main` on core 0.

### 2.3 Settle time (bench)

Two tiers, because a module boundary is a different event from an address
change:

| Transition | Contributors | Budget |
|---|---|---|
| Within a module | '161 CLK→Q + '251 addr→out | **100 ns** |
| Module boundary | above + decode→`/OE` + '251 output-enable + bus recharge | **200 ns** |
| Frame start | `/MR` pulse (~200 ns) + boundary-class settle | **400 ns** |

The `/MR` pulse is generous: a '161 needs ~5 ns of clear, and 200 ns is for the
harness, not the part. The POC's `esp_rom_delay_us(1)` is ~200× longer than
required.

The boundary tier is where the bus electrical design (§4) shows up. With
LVC-family parts it is settle-limited; with HC at 3.3 V it becomes slew-limited
and roughly triples.

### 2.4 Frame budget

Per channel: settle (100 ns) + read + CLK↑ + CLK↓ (~25 ns) ≈ **125 ns**.
Filament integrator: ~12 cycles/channel ≈ **50 ns**.
Boundaries: `num_modules - 1`, at +100 ns each. Frame start: 400 ns.

| Ch | Mod | Scan | Filament | Frame | Free-run | Core 1 @ 10 kHz |
|---|---|---|---|---|---|---|
| 16 | 1 | 2.4 µs | 0.8 µs | 3.2 µs | 313 kHz | 3.2% |
| 32 | 2 | 4.5 µs | 1.6 µs | 6.1 µs | 164 kHz | 6.1% |
| 64 | 4 | 8.7 µs | 3.2 µs | 11.9 µs | 84 kHz | 11.9% |
| 96 | 6 | 12.9 µs | 4.8 µs | 17.7 µs | 56 kHz | 17.7% |
| 128 | 8 | 17.1 µs | 6.4 µs | 23.5 µs | 43 kHz | **23.5%** |

Note the inversion at the top of the range: with `dedic_gpio` the integrator
costs more than a quarter of the frame, so the bottleneck has moved from I/O to
arithmetic. Not worth optimizing at 23% of a dedicated core, but that is where
the next gain lives — not in the scan loop.

For comparison, the POC's `gpio_set_level` + 1 µs settle gives ~1.9 µs/channel:
33 kHz at 16 channels, **5.5 kHz at 96**. Above the 2 kHz floor, but swinging 6×
across configurations — which is precisely the failure §2.5 fixes.

### 2.5 Paced sampling (FR-SCAN-8)

**Run at one fixed rate regardless of channel count. Default 10 kHz.**

Rationale:

- Feasible at 128 channels (23.5% of core 1), so behavior is identical on an
  8-channel bench rig and a 128-channel install. The filament time constants
  mean the same thing in both.
- 10 kHz gives ~20 samples across a 2 ms matrix strobe pulse, and 833 samples
  per 60 Hz half-cycle — conduction-angle resolution ~0.1%. Comfortable for both
  the duty estimate and the profiler.
- Uniform `dt` is what the leaky integrator assumes. A burst-then-sleep pacing
  scheme would reintroduce exactly the aliasing against the matrix strobe that
  the filament model exists to kill.
- Headroom to 20 kHz (47% of core 1) if the profiler ever wants finer
  conduction-angle resolution.

Mechanism: `gptimer` at Fs → task notification → scan task runs one frame.
Notification jitter is ~2–5 µs against a 100 µs period. Running the frame
directly in an IRAM-resident timer ISR is the zero-jitter fallback if the bench
disagrees. A busy-wait loop is not an option: it starves core 1's idle task.

### 2.6 Boot feasibility check

Config is not trusted. At startup:

1. Run ~256 frames, measure actual frame time `t_frame`.
2. `Fs_max = 1 / (t_frame / 0.6)` — 60% duty ceiling leaves margin for the
   integrator and interrupts.
3. Clamp the configured rate to `Fs_max`; log loudly if clamped.
4. Pass the **resulting** rate to `Filament::init()` and `Profiler::init()`, so
   the tau→coefficient math is derived from what the hardware actually does.

This is also what makes a slow front end (HC parts, long harness, a settle
retune) degrade gracefully instead of silently corrupting every time constant
in the system.

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

With the 1 kΩ bus pull: τ ≈ 150 ns, consistent with the 200 ns boundary settle.

### 4.2 Pull direction follows front-end polarity (HW-2)

The rule: **a floating or unpopulated bus must read *lamp off*.**

The front end is FET (inverting) → Schmitt inverter (inverting) = non-inverting
overall, so lamp on = logic high, and the pull is therefore **down**. This also
gives a safe pre-boot state: bus low, all lamps dark, before firmware runs.

If the front end were ever changed to inverting, the pull must flip to up and
`active_low` must flip with it. These two choices are one decision, not two.

### 4.3 Logic family (HW-3)

1 kΩ against 3.3 V is 3.3 mA of standing load on whichever '251 is driving.
That effectively selects the family:

| | 74HC251 @ 3.3 V | 74LVC251A @ 3.3 V |
|---|---|---|
| Output drive | ±2–4 mA (derated) | ±24 mA |
| t_pd | ~40–60 ns | ~5–8 ns |
| Slew 150 pF rail-to-rail | ~124 ns | ~20 ns |
| Holding against 1 kΩ | **marginal — may not reach valid V_OL/V_OH** | trivial |

HC at 3.3 V is fighting a 3.3 mA load with a ~3 mA budget. **Use LVC/LV.** The
alternative — a 4.7 kΩ pull with HC — costs ~1.5 µs of boundary settle, which at
8 boundaries adds ~12 µs/frame and roughly doubles the 128-channel frame time.

Since the harness carries no 5 V rail, there is no 5 V option to fall back to,
and the S3's lack of 5 V tolerance never becomes an issue.

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
| measured `t_frame` vs configured Fs | clamp Fs, log, use the clamped value for tau |
| `t_led(led_count)` vs `refresh_hz` at 70% occupancy | clamp `refresh_hz`, log |
| `led_count ≥ mapped channel count` | log unmapped channels |

## 7. To confirm on the bench

1. **Boundary settle.** Scope `DATA` at the far end of a full 8-module chain
   across a module transition. The 200 ns budget is the load-bearing estimate in
   §2.4.
2. **Bus capacitance and pull direction.** Verify ~150 pF and that the 1 kΩ is
   fitted as a pull-**down**.
3. **Front-end current per channel.** The 3 mA placeholder in §5.1 sets the
   entire power topology.
4. **CLK→DATA crosstalk** at full harness length, with and without series
   termination.
5. **Frame time vs. the model.** Compare measured `t_frame` at 1, 4, and 8
   modules against §2.4; the per-channel and per-boundary constants fall out of
   the slope and intercept.
6. **Coil-fire behavior.** Fire a flipper during a profiling window and confirm
   the classifier does not misbehave.
