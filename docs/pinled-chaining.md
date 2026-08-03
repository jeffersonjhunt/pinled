# PINLED Module Chaining

This document describes how PINLED multiplexer modules are chained, how the clock
is passed down the chain, and the protocol the master must follow to read every
line. It corresponds to schematic rev A (`pinled_module.kicad_sch`).

## Design goals

- Modules connect with a **4-conductor interface only**: `VCC`, `GND`, `DATA`, `CLK`.
  No reset wire, no token wire, no per-module addressing.
- Any number of identical modules can be chained; the master needs no configuration
  other than knowing the module count `N`.
- The chain must be robust against the classic 74-series hazards: gated-clock runt
  pulses and RCO decoding spikes.

## Interface

| Pin | Name | Direction | Description |
|-----|------|-----------|-------------|
| 1 | VCC | pass-through | 2 V – 6 V (74HC). All modules share the rail. |
| 2 | GND | pass-through | Common ground. |
| 3 | DATA | module → master | Shared 3-state bus. Exactly one multiplexer in the whole chain drives it at any time. |
| 4 | CLK | upstream → downstream | Master clock in; **conditionally forwarded** clock out. |

`CLK` is the only signal that differs between a module's input (J1) and output (J2)
connectors. `DATA` is bused straight through.

## How a module works

Each module contains a 74HC161 4-bit counter, a pair of 74HC251 8:1 multiplexers
(16 input lines per module), one half of a 74HC109 J-K̄ flip-flop used as a **DONE
latch**, a 74HC10 for gating, a 74HC14 Schmitt hex inverter, and a diode/RC
**activity detector**.

### Line selection

The counter's low three bits (QA–QC) drive the select inputs S0–S2 of both '251s
in parallel. QD chooses which '251 drives the bus: mux A (lines 0–7) when QD is
low, mux B (lines 8–15) when QD is high. The two Y outputs are wired together —
legal because the '251 output is 3-state and only one is ever enabled.

### Falling-edge clocking

The incoming clock is inverted by a Schmitt trigger section (74HC14), so **the
counter and the DONE flip-flop advance on the falling edge of `CLK`**. This is
deliberate and load-bearing:

1. DONE changes state while `CLK` is low, so the first pulse forwarded to the next
   module is always full width — no runt pulses at the handoff.
2. The downstream module's first *counting* edge arrives one half-period after its
   clock starts, so its line 0 gets a full time slot instead of being skipped.
3. At frame start, the first rising edge charges the activity detector (releasing
   the clears) a half-period before the first counting edge — no startup race.

### The DONE latch

The 74HC109 is wired J = RCO, K̄ = 1: it **sets on the same falling edge on which
the counter wraps 15 → 0** and then holds. The counter's ENP input is /DONE, so
after the wrap the counter parks at 0, already reset for the next frame.

RCO is used **only** at this synchronous input. It never gates a clock and never
drives an asynchronous control. The '161 datasheet's synchronous-operation
guarantee ("all flip-flops clocked simultaneously…") applies to the Q outputs;
RCO is *decoded* from the Qs (RCO = QA·QB·QC·QD·ENT), so output skew — e.g. on the
0111 → 1000 transition — can in principle produce a nanosecond-scale decoding
spike on RCO. Because RCO is only ever *sampled on a clock edge* here, such spikes
are harmless by construction.

### Clock forwarding

```
CLK_OUT = CLK_IN AND DONE
```

(implemented as a 3-input NAND with two inputs tied together, plus a Schmitt
inverter). While a module is counting, DONE is low and the downstream chain
receives no clock at all — downstream modules simply wait. The moment a module
finishes, it becomes transparent and the full clock passes through it. The clock
line therefore *is* the token: "you have been clocked" means "it is your turn."

### Bus arbitration (activity detector)

A module may drive `DATA` only while it is the active module:

```
/E_A = NOT (ACT AND /DONE AND /QD)
/E_B = NOT (ACT AND /DONE AND  QD)
```

`ACT` comes from a diode charge pump: incoming clock edges keep a capacitor
charged through D1; R1 bleeds it (τ = R1·C1 ≈ 10 ms). So:

- **Waiting modules** (no clock yet): ACT low → both muxes tri-stated.
- **Active module**: ACT high, DONE low → exactly one mux enabled.
- **Finished modules**: DONE high → both muxes tri-stated.

Exactly one driver on `DATA` chain-wide, at every instant. The master should hold
a ≈100 kΩ pulldown on `DATA` to define the bus during the brief handoff gaps.

`ACT` also drives the asynchronous clears (/CLR) of the counter and the DONE
flip-flop — see frame reset below.

## Chain behaviour

After a reset, all counters are at 0 and all DONE latches are clear.

1. The master starts toggling `CLK`. Only module 1 receives it. Module 1 scans
   its lines 0–15, one line per clock period.
2. On the falling edge that wraps module 1's counter 15 → 0, DONE₁ sets. Module 1
   releases `DATA`, parks at 0, and begins forwarding the clock.
3. Module 2 now receives the clock and repeats the process; the clock it forwards
   reaches module 3 only after module 2 finishes — and so on down the chain.
4. After 16·N clocks, every line has appeared on `DATA` exactly once and every
   module is parked at 0 with DONE set, transparently passing the clock.

### Frame reset — no wire required

The master ends a frame by simply **holding `CLK` low for ≥ 50 ms** (≥ 5τ). The
activity detectors in every module discharge, ACT falls, and the falling ACT
async-clears both the DONE latch and the counter. The next clock burst starts a
fresh frame. This also self-heals: a partial or glitched frame is fully cleaned
up by the idle gap, since the reset does not depend on any module state.

## Master protocol

- Drive `CLK` at ≥ 1 kHz (to keep the activity detectors charged) and at whatever
  upper rate your wiring supports; the chain adds roughly one AND-gate plus one
  Schmitt-inverter delay per module to the clock path.
- **Sample `DATA` just before each falling edge of `CLK`.** Sample *n* (1-based)
  is line ((n − 1) mod 16) of module ⌈n / 16⌉.
- Issue exactly 16·N clocks per frame, then idle `CLK` low ≥ 50 ms.
- Repeat.

## Timing summary

| Event | Edge | Effect |
|-------|------|--------|
| Falling edge, count < 15 | count + 1 | next line selected |
| Falling edge, count = 15 | wrap to 0, DONE sets | module releases bus, starts forwarding CLK |
| First forwarded rising edge | — | downstream ACT charges, downstream muxes arm |
| First forwarded falling edge | downstream count 0 → 1 | downstream line 0 has had a full slot |
| CLK idle > 5τ | ACT falls (all modules) | async clear of DONE + counter; frame ready to restart |

## Per-module bill of materials

| Ref | Part | Function |
|-----|------|----------|
| U1 | 74HC161 | 4-bit synchronous counter (ENT = 1, ENP = /DONE, load inputs grounded, /LOAD = 1) |
| U2, U3 | 74HC251 | 8:1 3-state multiplexers, lines 0–7 / 8–15 |
| U4 | 74HC109 (½ used) | DONE latch (J = RCO, K̄ = 1); unused half tied off |
| U5 | 74HC14 | clock inversion/buffering, CLK_OUT inversion, /QD, activity Schmitt (all 6 sections used) |
| U6 | 74HC10 | /E_A, /E_B gating and CLK_OUT NAND (all 3 gates used) |
| D1, R1, C1 | 1N4148, 100 kΩ, 100 nF | activity detector, τ ≈ 10 ms |
| C2–C6 | 100 nF | one decoupling cap per IC |
| C7 | 10 µF | bulk decoupling |
| J1, J2 | 1×4 header | chain in / chain out (VCC, GND, DATA, CLK) |
| J3 | 1×16 header | the 16 monitored lines |

## Design rationale (what this replaces and why)

An earlier revision gated the clock with XOR (local) and AND (forward) driven
directly by RCO. Three problems drove the redesign:

1. **XOR does not block a clock — it inverts it.** With carry high, XOR(CLK, 1) =
   /CLK, which still produces edges; the counter escapes state 15 on the clock's
   falling edge, making the effective modulus 15 and defeating the intended
   "freeze while carry is set" behaviour.
2. **Gating directly on RCO** exposes the design to RCO decoding spikes at exactly
   the moment the AND gate is transparent (CLK high, just after a rising edge).
3. **A frozen-at-15 module needs an external /MR** to recover (a fifth conductor),
   and idle modules at count 0 would fight the active module for the `DATA` bus.

The DONE-latch design keeps the original insight — the forwarded clock is the
token — while moving all decision-making onto registered, edge-sampled signals
and encoding the frame reset as a clock-idle timeout instead of a wire.
