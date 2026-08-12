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
| Channels (16/module) | 8 (single '165) | 128 |
| LEDs (1:1 with channels) | 8 | 128 |
| Harness length (100 mm hops) | 100 mm | 800 mm |

A single fixed `sample_rate_hz` constant cannot be correct across a 16× spread
in frame time. §2.5 is how that gets resolved.

## 2. Sense side

### 2.1 Topology

Modules are **identical and unaddressed**. Each carries two '165 shift registers
holding its 16 channels, chained `QH` → `SER` so that the whole harness is one
long shift register 16·N bits deep. `CLK` and `/PL` are bussed to every module;
`DATA` is point-to-point, one push-pull driver per hop. Full protocol in
`CHAINING.md`.

The master raises `/PL` — freezing every channel in the chain on that one edge —
then clocks 16·N bits out. There is no arming, no arbitration and nothing to
discard.

Three consequences worth carrying into the budgets below:

- **Capture is simultaneous.** Channel 0 and channel 127 are sampled on the same
  `/PL` edge, not 35 µs apart as in rev C. The filament integrator's aperture
  error across a frame drops to zero.
- **Clock skew is no longer cumulative, and no longer harmful.** Nothing
  regenerates the clock, so there is no per-hop gate delay to accumulate. What
  skew remains works *for* the design: `CLK` propagates master → downstream while
  `DATA` propagates downstream → master, so each module is clocked before the one
  feeding it and harness skew **adds** to the inter-device hold margin (§4.3).
- **A dead module still kills everything downstream**, but for a different
  reason — its `QH` stops driving and the receiving module's 10 kΩ pull-down
  holds the net at 0. The result is a clean, stable zero rather than a stalled
  chain, and modules nearer the master are unaffected.

The chain holds **no analog state**. There is no minimum inter-frame gap beyond
the '165's load pulse width, and no maximum mid-frame stall — a delayed frame is
merely late, never corrupt.

### 2.2 Why SPI + DMA (FR-SCAN-7)

In rev B this section argued correctness: the chain held its bus grant on a
capacitor that released after ~6 µs, so a bit-banged loop on FreeRTOS could
silently corrupt a frame. Rev C removed that cliff, and rev D has nothing
resembling it — the registers hold their snapshot indefinitely.

SPI is chosen for throughput and CPU cost, not safety:

| | Bit-bang | SPI + DMA |
|---|---|---|
| 128 ch at 10 kHz | ~23% of a core | **49%** (see §2.4 — the honest number) |
| Max clock | ~1 MHz realistically | 4–8 MHz, hardware-timed |
| Pacing | needs a timer | transaction cadence *is* the pacing |
| Jitter within a frame | present | none |

Mapping:

| SPI | Chain |
|---|---|
| `SCLK` | `CLK` |
| `MISO` | `DATA` |
| `CS` (positive polarity) | `/PL` |
| Mode | **2** (`CPOL=1, CPHA=0`) |

Driving `/PL` from `CS` is the neat part, and it survives from rev C unchanged:
`CS` is inactive between transactions and active during, so inverting its
polarity makes it low between frames (registers transparent, loading) and high
during one (registers frozen, shifting). The capture instant is hardware-timed,
needs no software, and cannot be forgotten.

**The single-transaction property is load-bearing.** §2.4 measures ~17 µs of
fixed driver overhead per SPI transaction. A frame that costs one transaction
pays that once; a design needing one transaction per module pays it eight times.
That was the deciding number against the **8× MCP23S17** expander option, which
requires `CS` to toggle between devices: 8 × (17 + 3.2) ≈ **162 µs** a frame, a
6.2 kHz free-run against a 10 kHz requirement — missing it by 2.7×, where a '165
chain does all 128 channels in one 49 µs transaction. This is the main reason
rev D chains shift registers rather than reading addressed expanders. Full
rejection rationale in `DOSSIER.md` §3.

**Gotcha:** the filament math below assumes the LX7 runs at **240 MHz**, and
ESP-IDF's default for `esp32s3` is **160 MHz**, which inflates per-channel
integrator cost by 1.5×. `sdkconfig.defaults` must set
`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` explicitly (NFR-7). This was not set on
the first S3 bring-up build and the boot log read `cpu freq: 160000000 Hz`;
confirm it there rather than assuming it.

### 2.3 Settle

At 4 MHz a bit slot is 250 ns, and the master samples in the middle of it — so
125 ns is the budget from a rising `CLK` edge to a valid level at the MCU pin.

| Contributor | Typical (LVC @ 3.3 V) |
|---|---|
| '165 CLK → `QH` | ~10 ns |
| `R2` (33 Ω) into the hop capacitance | ~5 ns |
| Harness flight time, one 100 mm hop | ~0.5 ns |
| **Total, module → master** | **~15 ns** |

Only *one* hop appears in that budget, which is the structural difference from
rev C. Data does not ripple through the chain: on each clock edge every register
simultaneously captures its neighbour's **pre-edge** output, so the path that has
to settle inside a bit slot is always one device to the next, never eight devices
end to end.

That leaves an inter-device **hold** requirement rather than a propagation chain:

```
t_h(SER)  ≤  t_pd(CLK→QH)  +  skew(k → k+1)
```

With `t_h` ≈ 0–3 ns and `t_pd` ≈ 10–20 ns, this holds by an order of magnitude
before the skew term is even counted — and the skew term is positive, because the
clock reaches module *k* before module *k+1* (§4.3, HW-14).

Two things that no longer appear in this budget at all, both from rev C: the
'251's output-enable turn-on time (there is no tri-state anything), and the arm
clock that existed to hide it. A '165 presents channel 0 at `QH` while `/PL` is
still low, so it has a full bit period before the first sample with no clock
spent on it.

### 2.4 Frame budget *(measured)*

A frame costs a **fixed ~17 µs of transaction overhead** plus the burst itself:

```
t_frame  ≈  17 µs  +  bits / f_spi
bits      =  16 · N                        (always a whole number of bytes)
```

The 17 µs was measured on hardware at three clock rates, and is essentially
independent of `f_spi`:

| `f_spi` | Bits | Burst | Measured frame | Implied overhead |
|---|---|---|---|---|
| 1 MHz | 12 | 12 µs | 30.0 µs | 18 µs |
| 2 MHz | 12 | 6 µs | 23.3 µs | 17.3 µs |
| 8 MHz | 12 | 1.5 µs | 18.2 µs | 16.7 µs |

> **This corrects an earlier version of this section**, which counted only burst
> time and claimed 6.4% CPU at 128 channels. That was wrong twice: it ignored
> per-transaction overhead entirely, and it assumed the CPU was free during the
> burst. The driver uses `spi_device_polling_transmit`, so the CPU is busy for
> the whole frame — interrupt-driven transmit frees it but costs ~37 µs of
> overhead instead of ~17 µs, which does not fit the target at 128 channels.
> The boot check (§2.6) exists precisely because this kind of estimate is
> unreliable; it caught this.

At the **4 MHz** default:

| Ch | Mod | Bits | Burst | Frame | Free-run | Fs at 60% | CPU @ 10 kHz |
|---|---|---|---|---|---|---|---|
| 16 | 1 | 16 | 4 µs | 21 µs | 48 kHz | 29 kHz | 21% |
| 32 | 2 | 32 | 8 µs | 25 µs | 40 kHz | 24 kHz | 25% |
| 64 | 4 | 64 | 16 µs | 33 µs | 30 kHz | 18 kHz | 33% |
| 96 | 6 | 96 | 24 µs | 41 µs | 24 kHz | 15 kHz | 41% |
| 128 | 8 | 128 | 32 µs | 49 µs | 20 kHz | **12 kHz** | **49%** |

At **8 MHz** the 128-channel case improves to a 16 µs burst, a 33 µs frame,
30 kHz free-run, 18 kHz clamped and 33% CPU.

10 kHz is met across the whole range, but with far less margin than the pre-
measurement model implied — 49% of core 1 at full extension, not 6.4%.
`scan_task` is pinned to core 1 and `render_task` to core 0, so that is
affordable, but it is the number to watch if anything else ever wants core 1.

> **Rev D is not meaningfully faster than rev C** — 49 µs against 52 µs at
> 128 channels is noise, and the whole difference is the 12 clocks saved by
> dropping the arm cycles. Rev D is worth having for part count, cable topology
> and simultaneous capture; it is *not* a throughput change. Do not let this
> table imply otherwise.

Raising `f_spi` buys frame time cheaply (the burst shrinks, the 17 µs does not).
Since rev D the bound is the '165's `fmax` and multi-drop clock integrity rather
than chain skew — see §4.3. Dropping to interrupt-driven transmit trades CPU for
wall-clock in the wrong direction for this workload.

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
transaction per frame at Fs; the hardware clocks 16·N bits and `CS` — wired as
`/PL` — reloads the chain between them.

There is no floor on the inter-frame gap and no ceiling on mid-frame stall, so
pacing jitter is a quality-of-measurement question rather than a correctness
one. A late frame widens `dt` for one sample; the integrator absorbs it — and
since rev D captures every channel on one `/PL` edge, a late frame is uniformly
late rather than internally skewed.

### 2.6 Boot feasibility check

Config is not trusted. At startup:

1. Check `16·N ≤ MAX_BITS` and that the configured Fs leaves room for the burst:
   `1/Fs ≥ 16·N/f_spi`. Clamp and log if not.
2. Run ~256 frames and measure the actual frame period, which also catches a
   mis-specified `f_spi` or a driver that silently rounds the clock divider.
3. Clamp the configured rate so a 60% CPU ceiling holds; log loudly if clamped.
4. Pass the **resulting** rate to `Filament::init()` and `Profiler::init()`, so
   the tau→coefficient math is derived from what the hardware actually does.

Rev B additionally had to refuse to start when the reset gap did not fit. That
check is gone with the RC: there is no gap to protect, so an over-ambitious rate
now simply gets clamped.

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
- **Power cap (FR-LED-7).** Supported by measurement as of 2026-08-11: 16 LEDs
  at 50% white drew 530 mA, i.e. ~66 mA/LED extrapolated to full, against the
  ~60 mA assumed here. See `HARDWARE.md`. 128 LEDs at the warm-white base is ~6 A / 30 W worst
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
| JST-SH harness, one 100 mm hop | ~6 pF |
| 2 × '165 input + one driver | ~15 pF |
| Module traces | ~8 pF |
| S3 input (master hop only) | ~3 pF |
| **Total, one hop** | **~25 pF** |

> **Rev D changed this number by an order of magnitude.** Rev C bussed `DATA`
> the whole 800 mm with 16 tri-state outputs hanging off it, for ~150 pF. Rev D
> loads only one hop at a time, so the driver sees ~25 pF. The 10 kΩ terminator
> gives τ ≈ 250 ns, which governs only how fast an *undriven* net decays — i.e.
> how quickly a missing module's channels settle to zero, not any live reading.

The one net that did *not* get easier is `CLK`, which is now multi-drop across
all 8 modules: ~45 pF of harness plus 16 register inputs. That is the load the
master's series resistor has to work into (§4.3).

### 4.2 Pull direction follows front-end polarity (HW-2)

The rule: **an undriven or unpopulated `DATA` net must read *lamp off*.**

The front end is FET (inverting) → Schmitt inverter (inverting) = non-inverting
overall, so lamp on = logic high, and the pull is therefore **down**. This also
gives a safe pre-boot state: all nets low, all lamps dark, before firmware runs.

Since rev D this rule does more work than it used to. A 10 kΩ pull-down sits at
*every* receiving end — one per module on `J2.3` plus one at the master — and it
is what makes a partly populated chain self-terminate (HW-11): missing modules
read as a hard zero rather than needing a plug or a build variant.

If the front end were ever changed to inverting, the pull must flip to up and
`active_low` must flip with it. These two choices are one decision, not two.

A **fourth** thing is bound to it since rev D: taking `/QH` instead of `QH`
would invert the serial path without moving the resistor, so a missing module
would report every channel *on*. HW-12 forbids it for exactly this reason.

There is a **third** thing bound to that same decision: the MCU's *internal*
pull on `DATA_IN`. `gpio_reset_pin()` leaves the internal pull-**up** enabled
and `gpio_set_direction()` does not clear it, so the naive setup contradicts the
rule above and an undriven bus reads *lamp on*. `lamp_scan::init()` therefore
configures the pin explicitly and derives the internal pull from `active_low`,
so all three flip together. The internal pull is ~45 kΩ and works alongside the
10 kΩ external terminator rather than replacing it.

### 4.3 Logic family (HW-3)

**This argument changed shape in rev D.** In rev C the binding constraint was
cumulative gate delay in a clock that was regenerated at every hop — a failure
that scaled with chain length. Rev D busses the clock, so that term is gone
entirely, and family choice becomes a preference rather than a correctness
requirement (HW-3).

**Skew now helps.** `CLK` propagates master → downstream; `DATA` propagates
downstream → master. Module *k* is therefore clocked *before* module *k+1*, and
latches *k+1*'s `QH` from before *k+1* had a chance to change it. Harness skew
adds to the hold margin:

```
t_h(SER)  ≤  t_pd(CLK→QH)  +  skew(k → k+1)
```

`t_h` ≈ 0–3 ns against `t_pd` ≈ 10–20 ns clears this before skew is counted. The
one way to break it is to **star-wire the clock** from the master, which can put
a downstream module *ahead* of the module feeding it and subtract from the margin
instead. Daisy the clock along the harness (HW-14).

**Drive.** The old 1 kΩ standing load is gone — rev D's terminator is 10 kΩ and
sees only 0.33 mA — so HC is no longer marginal on drive:

| | 74HC165 @ 3.3 V | 74LVC165 @ 3.3 V |
|---|---|---|
| Output drive | ±2–4 mA (derated) | ±24 mA |
| t_pd CLK → `QH` | ~30–45 ns | ~7–12 ns |
| Practical `fmax` | ~20–25 MHz | > 50 MHz |
| Verdict | **measured clean at 4 MHz** on a 4-chip breadboard rig; the right part for a **DIP bench build** | preferred for production |

**What binds instead: the multi-drop clock.** This is a genuine regression from
rev C and should be stated plainly. Rev C's `CLK` was point-to-point and
re-driven at every module; rev D hangs up to 16 register inputs and 8 connector
stubs on one net. That is the same criticism that disqualified the MCP23S17
option's four-signal bus — rev D reduces it to one fast signal (`/PL` is
effectively DC during a burst) but does not eliminate it.

Mitigations, in order of effect: 33–100 Ω series at the **master** (source
termination, HW-5); short stubs from connector to IC; and not reaching for clock
rate in the first place — 4 MHz already meets 10 kHz at 128 channels with 20%
margin (§2.4).

**Measured (bench, single module).** A clock sweep on the '161 + '151 rig — HC
parts, breadboard, plus an extra 74HC14 in the clock path — found the endpoint
delay directly:

| Clock | Period | Result |
|---|---|---|
| 1–16 MHz | ≥ 62.5 ns | 256/256 stable, correct channel |
| 20–26.7 MHz | ≤ 50 ns | 256/256 stable, **off by one channel** |
| 40 MHz | 25 ns | unstable |

The break between 62.5 ns and 50 ns puts the round-trip delay at **~55–60 ns**
for that rig, which is consistent with `delay < T_clk`.

> **Carry this measurement forward with care.** It was taken on rev C hardware
> ('161 + '151 + a 74HC14 in the clock path) and characterises *that* endpoint,
> not a '165 chain. What transfers is the method and the failure signature; the
> number itself must be re-measured on rev D. The `PINLED_SPI_SWEEP` diagnostic
> (FR-DIAG, `BRINGUP.md` §5) exists for exactly this.

**The failure mode is the finding — but it means something different now.** On
the rev C rig, exceeding the ceiling produced perfectly stable reads of
`line[k-1]`: every lamp one socket over, 256 times out of 256, looking exactly
like a wiring or mapping error rather than a timing error.

On rev D that same signature — a clean, stable, whole-frame one-bit shift — is
the symptom of the **wrong SPI mode**, not of an excessive clock. Exceeding the
clock ceiling on a '165 chain should instead present as *intermittent, noisy*
errors, because what fails first is signal integrity on the multi-drop clock
rather than a deterministic sampling-window violation. Check the mode before the
rate.

Because LVC is not a 5 V part, the harness carries 5 V and every module
regulates 3.3 V locally (HW-8). `DATA`, `CLK` and `/PL` are therefore 3.3 V
signals throughout and the S3's lack of 5 V tolerance never becomes an issue.

### 4.4 Signal integrity

800 mm of harness at LVC edge rates settles reflections in ~25 ns, well inside
the settle budget. The real risk is CLK→DATA crosstalk with a single ground
return in a 5-conductor cable:

- **33–100 Ω series termination on `CLK` at the master** to slow the edge and
  damp reflections. Since rev D this is the only multi-drop signal that switches
  at speed, so it is where the whole signal-integrity budget now lives.
- **33 Ω in series with each module's `QH`**, source-terminating the
  point-to-point `DATA` hop.
- The sampling order is already correct: mode 2 samples on the falling edge,
  half a bit slot after the rising edge that produced the data.
- **Per-module decoupling is mandatory, not optional.** 800 mm of thin wire is
  ~0.5–0.8 µH of loop inductance; a '165 cannot source a switching transient
  down the harness. Budget 100 nF per IC plus ~10 µF bulk per module.

## 5. Power & grounding

### 5.1 Distribution

Using a placeholder of **3 mA/channel** for the FET front end — *replace with
the measured value, it drives this whole section* — a 16-channel module is
~50 mA @ 3.3 V ≈ 175 mW, so eight modules ≈ **400 mA / 1.4 W**.

> **This covers the FET path only, and is not the whole front-end draw.** The
> original bulb is removed by design, so each channel may also carry a
> jumper-selectable **phantom load** — a power resistor supplying SCR holding
> current and keeping the unregulated lamp rail in spec (`DOSSIER.md` §7 item
> 4, `HARDWARE.md` front-end checklist). At the known ~470 Ω / 6.3 VAC figure
> that is ~13 mA per fitted channel, roughly **4× this placeholder**.
>
> It does **not** simply add to the totals below: phantom loads dissipate on the
> *lamp* rail, not the 3.3 V logic rail, so they are a separate budget — and a
> thermal problem on the module rather than a harness-current one. Fully
> populated at 128 channels it is on the order of 1.7 A drawn from the machine's
> own lamp supply, which is close to what the bulbs drew in the first place and
> is the honest price of keeping an SCR machine happy. Neither number is
> measured; both want settling on the Bally/Stern bench chain.

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
on the FET: the Schmitt and '165 are referenced to module-local ground, so the
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
| `1/Fs ≥ 16·N/f_spi` (burst fits in the period) | clamp Fs, log |
| measured `t_frame` vs configured Fs | clamp Fs, log, use the clamped value for tau |
| `t_led(led_count)` vs `refresh_hz` at 70% occupancy | clamp `refresh_hz`, log |
| `led_count ≥ mapped channel count` | log unmapped channels |

## 7. To confirm on the bench

1. ~~**SPI mode 2 is right.**~~ **Settled 2026-08-06.** A 4× 74HC165 rig
   (2 modules, 32 ch) at 4 MHz read `U1.D` as channel 4 and `U2.D` as channel
   12, exactly. The alignment is only possible if the first falling edge
   captures the pre-clock bit, so it fixes CPHA as well as CPOL. Same run
   settled bit order (`H` first, no byte reversal) and the `QH`→`SER` handoff
   within and across modules. Nothing below was affected — these all remain
   open, and items 2–4 need chain lengths the bench rig does not have.
2. **`/PL` reaches the far end, and its edge is clean.** With `CS` driving it,
   check the release-to-first-edge margin (`cs_ena_pretrans`) at module 8 across
   800 mm. This edge is now the capture instant for all 128 channels, so a slow
   or ringing edge skews the whole snapshot rather than one channel.
3. **Clock skew direction.** Scope `CLK` at module 8 against `SCLK` at the
   master and confirm module 8 is clocked *after* module 1 (HW-14). The sign
   matters more than the magnitude: late is safe, early eats hold margin.
4. **Multi-drop clock integrity.** The new binding constraint (§4.3). Scope
   `CLK` at the far end for ringing and double-clocking with and without the
   series resistor at the master.
5. ~~**Terminator behaviour.**~~ **Settled 2026-08-07.** Last module pulled
   live on the 4× '165 rig: its channels held a hard zero across ~4.9 M frames,
   including while the surviving module was actively switching. The 10 kΩ
   terminator holds against crosstalk, not just quiescence — HW-11 confirmed.
6. **Front-end current per channel.** The 3 mA placeholder in §5.1 sets the
   entire power topology.
7. **CLK→DATA crosstalk** at full harness length, with and without series
   termination.
8. **Frame time vs. the model.** Compare measured frame period at 1, 4, and 8
   modules against §2.4.
9. **Coil-fire behavior.** Fire a flipper during a profiling window and confirm
   the classifier does not misbehave.
