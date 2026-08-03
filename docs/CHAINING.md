# Module Chaining — `pinled` v2

How modules chain, how the clock is passed, and the protocol the master follows
to read every line. This is the authoritative description; `TIMING.md` derives
the numbers, `HARDWARE.md` covers the rest of the board.

Supersedes the earlier strapped-module-ID scheme, which was never built.

## Design goals

- **4 conductors only**: `VCC`, `GND`, `DATA`, `CLK`. No reset wire, no token
  wire, no per-module addressing, no ID straps.
- Modules are **identical and interchangeable**. The master needs no
  configuration beyond the module count `N`.
- Robust against the classic 74-series hazards: gated-clock runt pulses and
  `RCO` decoding spikes.

## Interface (HW-4)

4-pin JST-SH, ~100 mm hops, up to 8 modules / 800 mm.

| Pin | Name | Direction | Description |
|---|---|---|---|
| 1 | `VCC` | pass-through | **5 V**. Each module regulates its own 3.3 V locally (HW-8). |
| 2 | `GND` | pass-through | Common ground; single-point tie to lamp return (HW-6). |
| 3 | `DATA` | module → master | Shared 3-state bus. Exactly one mux drives it at any instant. |
| 4 | `CLK` | upstream → downstream | Master clock in; **conditionally forwarded** clock out. |

`CLK` is the only signal that differs between a module's input (`J1`) and output
(`J2`) connectors. `DATA`, `VCC` and `GND` bus straight through.

## How a module works

Per module: a '161 4-bit counter, two '251 3-state 8:1 muxes (16 lines), half a
'109 J-K̄ flip-flop as a **DONE latch**, a '10 triple NAND for gating, a '14
Schmitt hex inverter, and a diode/RC **activity detector**. All LVC (HW-3).

### Line selection

The counter's low three bits `QA..QC` drive `S0..S2` on both '251s in parallel.
`QD` selects which mux drives the bus: mux A (lines 0–7) when `QD` is low, mux B
(lines 8–15) when high. The two `Y` outputs are wired together — legal because
the '251 output is 3-state and only one is ever enabled.

### Falling-edge clocking

The incoming clock is inverted by a Schmitt section, so **the counter and the
DONE latch advance on the falling edge of `CLK`**. This is load-bearing:

1. DONE changes state while `CLK` is low, so the first pulse forwarded to the
   next module is always full width — no runt at the handoff.
2. The downstream module's first *counting* edge arrives a half-period after its
   clock starts, so its line 0 gets a full slot instead of being skipped.
3. At frame start the first rising edge charges the activity detector, releasing
   the clears a half-period before the first counting edge — no startup race.

### The DONE latch

The '109 is wired `J = RCO`, `K̄ = 1`: it **sets on the same falling edge that
wraps the counter 15 → 0**, then holds (`J = 0, K = 0`). The counter's `ENP` is
`/DONE`, so after the wrap it parks at 0, already reset for the next frame.

`RCO` is used **only** at this synchronous input. It never gates a clock and
never drives an asynchronous control. `RCO` is *decoded* from the Q outputs
(`RCO = QA·QB·QC·QD·ENT`), so output skew — e.g. on `0111 → 1000` — can produce
a nanosecond-scale spike. Because `RCO` is only ever sampled on a clock edge,
such spikes are harmless by construction.

### Clock forwarding — the clock is the token

```
CLK_OUT = CLK_IN AND DONE
```

(a 3-input NAND with two inputs tied, plus a Schmitt inverter). While a module
is counting, DONE is low and the downstream chain receives **no clock at all**.
The moment a module finishes it becomes transparent and the full clock passes
through. "You have been clocked" means "it is your turn" — no addressing needed.

### Bus arbitration (activity detector)

```
/E_A = NOT (ACT AND /DONE AND /QD)
/E_B = NOT (ACT AND /DONE AND  QD)
```

`ACT` comes from a diode charge pump: incoming clock edges keep `C1` charged
through `D1`; `R1` bleeds it. So:

- **Waiting** (no clock yet): ACT low → both muxes tri-stated.
- **Active**: ACT high, DONE low → exactly one mux enabled.
- **Finished**: DONE high → both muxes tri-stated.

Exactly one driver chain-wide at every instant. `ACT` also drives the
asynchronous clears of the counter and the DONE latch.

## Frame reset — no wire required

The master ends a frame by **holding `CLK` low for ≥ 5τ**. Every activity
detector discharges, `ACT` falls, and that async-clears both the DONE latch and
the counter. The next clock burst starts a fresh frame.

This self-heals: a partial or glitched frame is fully cleaned by the idle gap,
because the reset depends on no module state. It is also why there is no `/MR`
conductor.

## Timing constants

τ = `R1·C1` is pulled three ways and all three matter:

| Constraint | Requirement |
|---|---|
| Hold during a frame | `T_clk << τ` — ACT must not droop between clock edges |
| Frame reset | `5τ ≤` the inter-frame idle gap the master can afford |
| Startup | `C1` must charge inside one clock high-phase |

That last one is easy to miss and it is what forces small values. Charging `C1`
through `D1` and an LVC output (~25 Ω) must complete in well under a half
period.

**Chosen values:**

| | Value | Result |
|---|---|---|
| `R1` | 12 kΩ | |
| `C1` | 470 pF | τ ≈ 5.6 µs, charge τ ≈ 12 ns |
| SPI clock | 2 MHz | `T_clk` = 500 ns, droop ≈ 9%/period |
| Idle gap | ≥ 28 µs | 5τ |

With a peak of ~2.7 V on `C1` (3.3 V less a diode drop) and an LVC Schmitt
`V_T-` ≈ 0.9 V, `ACT` falls about **6.2 µs** after the last clock edge. That is
the chain's stall tolerance: a gap in clocking longer than that mid-frame
silently resets everything. It is why the master clocks from **SPI + DMA** and
not from a CPU loop — see `TIMING.md`.

> These are calculated from datasheet typicals and want bench confirmation.
> `ACT` fall time and the first-bit validity at frame start are the two to
> measure.

## Master protocol

1. Hold `CLK` low ≥ 5τ (28 µs) to reset the chain.
2. Issue exactly **16·N clocks**, sampling `DATA` just before each falling edge.
3. Repeat.

Sample *n* (1-based) is line `(n−1) mod 16` of module `⌈n/16⌉`.

### SPI mapping (FR-SCAN-7)

The burst maps onto a single SPI transaction with no CPU involvement:

| SPI | Chain |
|---|---|
| `SCLK` | `CLK` |
| `MISO` | `DATA` |
| `MOSI` | unused (receive-only transaction) |
| `CS` | unused (`spics_io_num = -1`) |
| Mode | **1** (`CPOL=0, CPHA=1`) — shift on rising, sample on falling |

Mode 1 samples on the falling edge, which is exactly "just before the falling
edge" once setup time is accounted for: the sampled value is the pre-edge one,
so sample 1 is line 0 of module 1 as specified. `CPOL=0` also means `SCLK` idles
low between transactions, which *is* the frame reset.

16·N bits is always a whole number of bytes (2·N), and with MSB-first ordering:

```
raw[ch] = (rx[ch / 8] >> (7 - (ch % 8))) & 1
```

A DMA transaction is gapless by construction, so the ≤6.2 µs stall tolerance is
never tested mid-frame. The transaction cadence sets the sample rate directly,
which is why no separate pacing timer is required.

## Chain behaviour

1. Master starts clocking. Only module 1 receives it; it scans lines 0–15.
2. On the falling edge that wraps its counter, DONE₁ sets. Module 1 releases
   `DATA`, parks at 0, and begins forwarding the clock.
3. Module 2 now receives the clock and repeats. Its forwarded clock reaches
   module 3 only after it finishes, and so on.
4. After 16·N clocks every line has appeared on `DATA` exactly once and every
   module is parked with DONE set, transparently passing the clock.

| Event | Edge | Effect |
|---|---|---|
| count < 15 | falling | count + 1, next line selected |
| count = 15 | falling | wrap to 0, DONE sets, bus released, clock forwarding starts |
| first forwarded rising | — | downstream ACT charges, muxes arm |
| first forwarded falling | — | downstream count 0 → 1; its line 0 had a full slot |
| `CLK` idle > 5τ | — | ACT falls chain-wide; async clear of DONE + counters |

## Failure modes

No addressing means no missing-ID or duplicate-ID faults, but the topology
introduces a different one:

| Fault | Symptom | Notes |
|---|---|---|
| Module never asserts DONE | **all downstream channels dead** | The clock never forwards. Worse than the old scheme, where one bad module cost only its own 16 channels. |
| `N` configured too high | trailing samples read the bias | Indistinguishable from lamps being off |
| `N` configured too low | trailing modules never scanned | Silent |
| Mid-frame stall > ~6.2 µs | whole frame corrupt | Cannot happen with DMA; would with a CPU loop |
| Chain longer than clock skew allows | far modules sampled in the wrong bit slot | Chain-length dependent — see `TIMING.md` |

There is no reliable way to auto-detect the true module count: over-clocking a
short chain reads the bias resistor, which looks exactly like lamps being off.
`N` stays configuration.

## Per-module bill of materials

| Ref | Part | Function |
|---|---|---|
| U1 | 74LVC161 | counter (`ENT`=1, `ENP`=`/DONE`, load inputs grounded, `/LOAD`=1) |
| U2, U3 | 74LVC251 | 8:1 3-state muxes, lines 0–7 / 8–15 |
| U4 | 74LVC109 (½) | DONE latch (`J`=`RCO`, `K̄`=1); unused half tied off |
| U5 | 74LVC14 | clock inversion, `CLK_OUT` inversion, `/QD`, activity Schmitt |
| U6 | 74LVC10 | `/E_A`, `/E_B`, `CLK_OUT` NAND |
| U7 | 3.3 V LDO | local regulation from the 5 V rail (HW-8) |
| U8–U10 | 74LVC14 ×3 | front-end inverting Schmitts, 16 channels |
| Q1–Q16 | N-ch MOSFET | front-end level shift, inverting (HW-1) |
| D1, R1, C1 | 1N4148, 12 kΩ, 470 pF | activity detector, τ ≈ 5.6 µs |
| — | 100 Ω | series termination on `CLK_OUT` at the source (HW-5) |
| C | 100 nF per IC + 10 µF bulk | decoupling (HW-5) |
| J1, J2 | 4-pin JST-SH | chain in / chain out |

The 1 kΩ `DATA` bias (HW-2) lives at the **master**, not on the module — one
per chain, not one per module.

## Schedule of changes from schematic rev A

Rev A (`pinled_module.kicad_sch`) is the 74HC reference. To reach rev B:

1. **All logic → LVC.** 74HC at 3.3 V is too slow in the forwarded clock path;
   see `TIMING.md`. Verify sourcing for the '109 in a fast family — if
   unavailable, the fallback is a D-flop plus an OR gate, which costs a package
   since the '10 is fully used.
2. **`R1` 100 kΩ → 12 kΩ, `C1` 100 nF → 470 pF.** Rev A's τ = 10 ms caps the
   frame rate at 20 Hz.
3. **Add U7**, a 3.3 V LDO with input/output caps. `VCC` on the harness becomes
   the 5 V rail; all logic runs from the local 3.3 V.
4. **Remove J3.** The 16 lines are internal nets from the on-board front end,
   not a header.
5. **Add the front end**, 16 × (MOSFET + inverting Schmitt + divider/clamp) per
   HW-1. Note this needs its own '14 packages — U5's six sections are fully
   consumed by the scan logic.
6. **J1/J2 → 4-pin JST-SH**, and add ~100 Ω series on `CLK_OUT`.
7. Confirm no `DATA` bias resistor is fitted on the module.
