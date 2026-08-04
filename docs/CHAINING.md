# Module Chaining — `pinled` v2 (rev C)

How modules chain, how the clock is passed, and the protocol the master follows
to read every line. This is the authoritative description; `TIMING.md` derives
the numbers, `HARDWARE.md` covers the rest of the board.

**Rev C** replaces the RC activity detector with a `STARTED` flip-flop and an
explicit `/MR` conductor. Rev B (4-wire, RC) and the earlier strapped-module-ID
scheme were never built.

## Design goals

- Modules are **identical, interchangeable and unaddressed**. Nothing is
  strapped, jumpered or configured per module; the master needs only the module
  count `N`.
- **No analog timing anywhere.** Every decision is registered and edge-sampled.
- Robust against the classic 74-series hazards: gated-clock runt pulses and
  `RCO` decoding spikes.

## Interface (HW-4)

5-pin JST-SH, ~100 mm hops, up to 8 modules / 800 mm.

| Pin | Name | Direction | Description |
|---|---|---|---|
| 1 | `VCC` | pass-through | **5 V**. Each module regulates its own 3.3 V locally (HW-8). |
| 2 | `GND` | pass-through | Common ground; single-point tie to lamp return (HW-6). |
| 3 | `DATA` | module → master | Shared 3-state bus. Exactly one mux drives it at any instant. |
| 4 | `CLK` | upstream → downstream | Master clock in; **conditionally forwarded** clock out. |
| 5 | `/MR` | master → all | Async clear, active low. Bussed to every module. |

`CLK` is the only signal that differs between a module's input (`J1`) and output
(`J2`) connectors. `DATA`, `/MR`, `VCC` and `GND` bus straight through.

## Why rev C dropped the RC

The rev B activity detector did two jobs: frame reset, and telling a module it
was its turn to drive the bus. `/MR` only replaces the first — after a reset
every module has `DONE = 0`, so gating `/OE` on `/DONE` alone would have all of
them driving `DATA` at once. A module still needs local evidence that its turn
has started.

That evidence does not need a capacitor. It needs one flip-flop, and the '109 is
a *dual* — rev B used half of it. What the change bought:

| Removed | Consequence |
|---|---|
| τ and the ≥5τ idle gap | No dead time between frames; frame rate is set by the clock alone |
| The ~6 µs stall cliff | A mid-frame stall is now harmless. SPI + DMA is a performance choice, not a correctness requirement |
| `D1`, `R1`, `C1` | No analog time constant to drift with temperature in a warm cabinet |
| The single-step ban | The chain can be clocked arbitrarily slowly, so the bring-up diagnostics work on production hardware |

Cost: a fifth conductor, one more MCU GPIO, and one warm-up clock per module.

## How a module works

Per module: a '161 4-bit counter, two '251 3-state 8:1 muxes (16 lines), a '109
dual J-K̄ (both halves used), a '10 triple NAND for gating, a '14 Schmitt hex
inverter, and a local 3.3 V LDO. All LVC (HW-3).

### Line selection

The counter's low three bits `QA..QC` drive `S0..S2` on both '251s in parallel.
`QD` selects which mux drives the bus: mux A (lines 0–7) when `QD` is low, mux B
(lines 8–15) when high. The two `Y` outputs are wired together — legal because
the '251 output is 3-state and only one is ever enabled.

### Falling-edge clocking

The incoming clock is inverted by a Schmitt section, so **the counter and both
flip-flops advance on the falling edge of `CLK`**. This is load-bearing:

1. `DONE` changes state while `CLK` is low, so the first pulse forwarded to the
   next module is always full width — no runt at the handoff.
2. It puts the counter transition half a period away from the master's sampling
   edge, which is what makes a mid-window sample possible at all.

### The two latches

Both halves of the '109 are clocked by the inverted clock and cleared by `/MR`.

| Half | Wiring | Behaviour |
|---|---|---|
| `DONE` | `J = RCO`, `K̄ = 1` | Sets on the falling edge that wraps the counter 15 → 0, then holds |
| `STARTED` | `J = 1`, `K̄ = 1` | Sets on the **first** clock edge this module receives, then holds |

`RCO` is used **only** at that synchronous `J` input. It never gates a clock and
never drives an asynchronous control. `RCO` is *decoded* from the Q outputs
(`RCO = QA·QB·QC·QD·ENT`), so output skew — e.g. on `0111 → 1000` — can produce
a nanosecond-scale spike. Because `RCO` is only ever sampled on a clock edge,
such spikes are harmless by construction.

### The counter's two enables do two jobs

The '161 has two synchronous count enables, and rev C uses them separately
rather than needing an extra gate:

```
ENP = STARTED     // do not count until this module's turn has begun
ENT = /DONE       // stop counting once finished (also gates RCO, which is fine
                  //  -- DONE is already latched by then)
```

`ENP = STARTED` is what gives line 0 a full bit period. Both enables are
*synchronous*, so on the first clock edge the '161 samples the **old**
`STARTED = 0` and does not count, while the '109 sets `STARTED` on that same
edge. The module therefore spends its first clock arming and its next sixteen
presenting lines 0–15.

### Bus arbitration

```
/E_A = NOT (STARTED AND /DONE AND /QD)
/E_B = NOT (STARTED AND /DONE AND  QD)
```

- **Waiting** (no clock yet): `STARTED` low → both muxes tri-stated.
- **Active**: `STARTED` high, `DONE` low → exactly one mux enabled.
- **Finished**: `DONE` high → both muxes tri-stated.

Exactly one driver chain-wide at every instant.

### Clock forwarding — the clock is the token

```
CLK_OUT = CLK_IN AND DONE
```

(a 3-input NAND with two inputs tied, plus a Schmitt inverter, then ~100 Ω
series per HW-5). While a module is counting, `DONE` is low and the downstream
chain receives **no clock at all**. The moment a module finishes it becomes
transparent and the full clock passes through. "You have been clocked" means "it
is your turn" — no addressing needed.

## Frame structure

Each module consumes **17 clocks**: one to arm, sixteen to present its lines.

```
/MR  ──┐_____┌──────────────────────────────────────────────────────
       └ low ┘  clears all counters and both latches, chain-wide

CLK        ┌─┐ ┌─┐ ┌─┐  ...  ┌─┐   ┌─┐ ┌─┐  ...  ┌─┐
            1   2   3         17    18  19        34
            │   └─────────────┘     │   └──────────┘
           arm    lines 0..15      arm   lines 0..15
                module 1                 module 2
```

Sample *i* (0-based) with `i mod 17 == 0` is an arm clock and carries no data.
Otherwise it is line `(i mod 17) - 1` of module `i / 17`.

Total clocks = **17·N**. At 8 modules that is 136, exactly 17 bytes.

## Master protocol

1. Pulse `/MR` low, then release it.
2. Issue **17·N clocks**, sampling `DATA` on each rising edge.
3. Discard every 17th sample (the arm clocks).
4. Repeat. There is no minimum gap between frames beyond the `/MR` pulse width.

### SPI mapping (FR-SCAN-7)

| SPI | Chain |
|---|---|
| `SCLK` | `CLK` |
| `MISO` | `DATA` |
| `MOSI` | unused (receive-only transaction) |
| `CS` | `/MR`, with **positive polarity** (`SPI_DEVICE_POSITIVE_CS`) |
| Mode | **0** (`CPOL=0, CPHA=0`) — sample on rising |

Mode 0 is correct because the counter advances on the **falling** edge, so line
*n* is valid from `fall(n)` to `fall(n+1)` and a rising-edge sample lands in the
*middle* of that window. Mode 1 would sample on the falling edge — exactly the
counter transition — and is wrong.

> **Measured.** Verified empirically, not reasoned from the SPI spec. On a rig
> clocking the '161 *directly* (counter on the rising edge), modes 0 and 1 both
> shifted by one channel; adding an inverter so the counter advanced on the
> falling edge made mode 0 read correctly. See `BRINGUP.md` §7.

Wiring `/MR` to `CS` with positive polarity is worth doing: `CS` sits inactive
between transactions and asserts during, so with the polarity inverted it is
**low between frames (clearing) and high during (counting)**. The frame reset
becomes hardware-timed with no software involvement and no minimum-gap rule,
and `cs_ena_pretrans` sets the release-to-first-edge margin.

Bits round up to whole bytes; surplus clocks past 17·N are harmless because
every module is then finished and tri-stated. With MSB-first ordering:

```
bit i   = (rx[i / 8] >> (7 - (i % 8))) & 1
channel = (i / 17) * 16 + (i % 17) - 1      // skip i % 17 == 0
```

## Chain behaviour

1. Master releases `/MR` and starts clocking. Only module 1 receives it.
2. Module 1's first clock sets `STARTED₁` without counting; its next sixteen
   present lines 0–15.
3. On the falling edge that wraps its counter, `DONE₁` sets. Module 1 releases
   `DATA`, stops counting (`ENT` low), and begins forwarding the clock.
4. Module 2 receives its first clock on the next rising edge and repeats.
5. After 17·N clocks every line has appeared on `DATA` exactly once.

| Event | Edge | Effect |
|---|---|---|
| First clock received | falling | `STARTED` sets; counter does **not** advance (synchronous `ENP`) |
| count < 15 | falling | count + 1, next line selected |
| count = 15 | falling | wrap to 0, `DONE` sets, bus released, clock forwarding starts |
| `/MR` low | async | all counters and both latches cleared chain-wide |

## Failure modes

| Fault | Symptom | Notes |
|---|---|---|
| Module never asserts DONE | **all downstream channels dead** | The clock never forwards. Worse than a per-module data line, where one bad module cost only its own 16 channels. |
| `N` configured too high | trailing samples read the bias | Indistinguishable from lamps being off |
| `N` configured too low | trailing modules never scanned | Silent |
| Chain longer than clock skew allows | far modules read `line[k-1]`, **perfectly stable** | Measured signature: not noise, a clean one-channel shift. See `TIMING.md` §4.3 |
| `/MR` open or stuck high | counters never clear; frames drift out of alignment | New in rev C — rev B could not have this fault |

There is no reliable way to auto-detect the true module count: over-clocking a
short chain reads the bias resistor, which looks exactly like lamps being off.
`N` stays configuration.

## Per-module bill of materials

| Ref | Part | Function |
|---|---|---|
| U1 | 74LVC161 | counter (`ENP` = `STARTED`, `ENT` = `/DONE`, `/LOAD` = 1, load inputs grounded, `/CLR` = `/MR`) |
| U2, U3 | 74LVC251 | 8:1 3-state muxes, lines 0–7 / 8–15 |
| U4 | 74LVC109 | **both halves**: `DONE` latch (`J` = `RCO`) and `STARTED` latch (`J` = 1) |
| U5 | 74LVC14 | clock inversion, `CLK_OUT` inversion, `/QD` (3 of 6 sections used) |
| U6 | 74LVC10 | `/E_A`, `/E_B`, `CLK_OUT` NAND |
| U7 | 3.3 V LDO | local regulation from the 5 V rail (HW-8) |
| R1 | 100 Ω | series termination on `CLK_OUT` at the source (HW-5) |
| C1–C6 | 100 nF | one decoupling cap per logic IC (HW-5) |
| C7 | 10 µF | bulk decoupling |
| C8, C9 | 1 µF | LDO input / output |
| J1, J2 | 5-pin JST-SH | chain in / chain out |

Front end (16 × MOSFET + inverting Schmitt, HW-1) drives `LINE0..LINE15` and
lives on its own sheet. Note it needs its own '14 packages — U5's three spare
sections are nowhere near enough for 16 channels.

The 1 kΩ `DATA` bias (HW-2, HW-11) lives at the **master**, one per chain.

## Net list

Every connection, for building or for checking the schematic.

| Net | Members |
|---|---|
| `VCC5` | J1.1, J2.1, U7.VIN, C8 |
| `V3V3` | U7.VOUT, C9, VCC of U1–U6, C1–C7 |
| `GND` | J1.2, J2.2, U7.GND, GND of U1–U6, all caps, U1.A–D |
| `DATA` | J1.3, J2.3, U2.Y, U3.Y |
| `CLK_IN` | J1.4, U5A.in, U6C.in1, U6C.in2 |
| `CLKOUT_N` | U6C.out, U5C.in |
| `CLKOUT_RAW` | U5C.out, R1.1 |
| `CLK_OUT` | R1.2, J2.4 |
| `MR_N` | J1.5, J2.5, U1.`/CLR`, U4A.`/CLR`, U4B.`/CLR` |
| `CLK_INT` | U5A.out, U1.CLK, U4A.CLK, U4B.CLK |
| `QA`, `QB`, `QC` | U1.QA/QB/QC → U2.S0–S2 and U3.S0–S2 |
| `QD` | U1.QD, U5B.in, U6B.in3 |
| `QD_N` | U5B.out, U6A.in3 |
| `RCO` | U1.RCO, U4A.J |
| `DONE` | U4A.Q, U6C.in1, U6C.in2 |
| `DONE_N` | U4A.`/Q`, U1.ENT, U6A.in2, U6B.in2 |
| `STARTED` | U4B.Q, U1.ENP, U6A.in1, U6B.in1 |
| `EA_N` | U6A.out, U2.`/E` |
| `EB_N` | U6B.out, U3.`/E` |
| `LINE0..7` | U2.D0–D7 (from the front-end sheet) |
| `LINE8..15` | U3.D0–D7 (from the front-end sheet) |

Tied high to `V3V3`: U1.`/LOAD`, U4A.`K̄`, U4A.`/PRE`, U4B.`J`, U4B.`K̄`,
U4B.`/PRE`.

Note `CLK_IN` drives both inputs of the `CLK_OUT` NAND together with `DONE` —
`U6C` is a 3-input gate with `DONE` on one input and `CLK_IN` on the other two.

## Artifacts

| File | What |
|---|---|
| `chain_timing.svg` | Waveforms: arm clock, handoff, `/MR` frame reset |
| `pinled_module_revC.kicad_sch` | Rev C schematic |

> **The schematic is generated, not drawn.** It was emitted programmatically
> from the symbol library with stub-and-label connectivity, and **no KiCad was
> available to open it or run ERC**. The net list above is the authority; treat
> the schematic as a convenience that needs verifying before a board is spun.
>
> Expected ERC output when you do open it: `LINE0..LINE15` are single-pin nets
> (they cross to the front-end sheet), and `VCC5` / `V3V3` / `GND` are global
> labels rather than power symbols, so power-pin-not-driven warnings are normal.
> `DATA` carrying two drivers is fine — the '251 `Y` pins are typed `tri_state`.
> Layout is utilitarian: symbols on a grid, every pin on a labelled stub.
