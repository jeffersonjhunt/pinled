# Module Chaining — `pinled` v2 (rev D)

How modules chain, how a frame is captured, and the protocol the master follows
to read every line. This is the authoritative description; `TIMING.md` derives
the numbers, `HARDWARE.md` covers the rest of the board.

**Rev D replaces the whole scan mechanism.** The counter, the muxes, the two
latches, the gating and the clock-as-token protocol are gone, and in their place
each module carries two **74x165 parallel-in / serial-out shift registers**. Rev
C (counter + tri-state muxes + `STARTED`/`DONE` latches + `/MR`) was fully
specified and partially bench-validated but never built as a module; rev B
(4-wire, RC activity detector) and the earlier strapped-module-ID scheme were
never built either.

## Design goals

Unchanged from rev C:

- Modules are **identical, interchangeable and unaddressed**. Nothing is
  strapped, jumpered or configured per module; the master needs only the module
  count `N`.
- **No analog timing anywhere.** Every decision is registered and edge-sampled.
- One SPI transaction per frame, so the CPU pays the driver's fixed
  per-transaction cost once and not once per module.

New in rev D:

- **The chain self-terminates.** A partially populated harness reads as "lamps
  off" beyond the last fitted module, with no terminator plug and no build
  variant.
- **One capture instant for all 128 channels**, rather than a sweep across the
  length of the frame.

## Why rev D

Rev C worked, but it spent six logic ICs per module building a distributed state
machine whose only job was to take turns on a shared bus. A shift register does
the same job with no state machine at all, because taking turns is what a shift
register *is*.

| Rev C | Rev D |
|---|---|
| '161 counter + 2× '251 + '109 + '10 + '14 (6 ICs) | 2× '165 (2 ICs) |
| Shared 3-state `DATA` bus, arbitrated | Point-to-point `DATA`, one push-pull driver per hop |
| Clock forwarded conditionally (`CLK_OUT = CLK_IN AND DONE`) | Clock bussed to everyone |
| Cumulative gate skew, fails as a function of chain length | No per-hop gate delay; skew *adds* margin (see below) |
| 17 clocks/module — one spent arming | 16 clocks/module, nothing discarded |
| Channel 0 and channel 127 sampled 35 µs apart | All channels captured on one edge |
| `N` too high reads the master's bias resistor | `N` too high reads a hard 0 from a local pull-down |

The costs are real and listed under [Failure modes](#failure-modes): `CLK` is now
multi-drop rather than regenerated at each hop, and a reversed connector shorts
two push-pull outputs together.

## Interface (HW-4)

5-pin JST-SH, ~100 mm hops, up to 8 modules / 800 mm. **Same connector and same
pin count as rev C**, with `/MR` replaced by `/PL` and `DATA` changing direction.

| Pin | Name | Direction | Description |
|---|---|---|---|
| 1 | `VCC` | pass-through | **5 V**. Each module regulates its own 3.3 V locally (HW-8). |
| 2 | `GND` | pass-through | Common ground; single-point tie to lamp return (HW-6). |
| 3 | `DATA` | **downstream → upstream** | Serial data toward the master. `QH` out on `J1`, `SER` in on `J2`. |
| 4 | `CLK` | pass-through | Shift clock, bussed to every module. |
| 5 | `/PL` | pass-through | Parallel load, active low. Bussed to every module. |

`DATA` is the only signal that differs between a module's upstream connector
(`J1`, toward the master) and its downstream connector (`J2`). `CLK`, `/PL`,
`VCC` and `GND` bus straight through. This is the mirror image of rev C, where
`CLK` was the signal that differed and `DATA` was bussed.

> **`J1` and `J2` are not interchangeable.** `J1.DATA` is an *output*; `J2.DATA`
> is an *input*. Plugging a module in backwards ties two `QH` outputs together.
> Key or gender the connectors so it cannot happen, and fit `R2` (below) so that
> if it does, the collision is current-limited rather than destructive.

## How a module works

Per module: two '165 shift registers, a local 3.3 V LDO, two resistors and
decoupling. That is the entire module besides the front end.

### The two operations

A '165 does two different things on two different signals, and they never
overlap because parallel load overrides shifting.

| `/PL` | Behaviour |
|---|---|
| **low** | The eight parallel inputs are loaded **transparently**. This is level-sensitive, not edge-triggered — the register simply follows the front end. `CLK` is ignored. |
| **high** | The register is frozen and `CLK` shifts its contents toward `QH`, one bit per **rising** edge. `SER` feeds in at the far end. |

Because the load is a level rather than a pulse, the capture instant is the
**rising edge of `/PL`** — the moment the register stops following its inputs.
Every '165 in the chain sees that edge within a few nanoseconds of every other,
so a frame is a genuine simultaneous snapshot of all 16·N channels.

### Shift direction and channel order

A '165 shifts `A → B → … → H → QH`, so the **first** bit to appear at `QH` after
a load is whatever sits on input `H`, and the last is `A`.

Within a module, `U1` holds channels 0–7 and sits nearest the master; `U2` holds
channels 8–15 and feeds `U1.SER`. Wire the front end so that **channel 0 lands on
`U1` pin `H`**, counting down to channel 7 on pin `A`:

| Channel (module-local) | '165 input | Pin |
|---|---|---|
| 0 / 8 | `H` | 6 |
| 1 / 9 | `G` | 5 |
| 2 / 10 | `F` | 4 |
| 3 / 11 | `E` | 3 |
| 4 / 12 | `D` | 14 |
| 5 / 13 | `C` | 13 |
| 6 / 14 | `B` | 12 |
| 7 / 15 | `A` | 11 |

That ordering is what makes the received stream come out in plain ascending
channel order, MSB-first, so the driver's unpack loop is a straight bit walk with
no reversal. Wiring channel 0 to `A` instead would work electrically and cost a
byte-reverse in the hot loop.

### Chain termination — how partial population works

Every `DATA` net gets a **10 kΩ pull-down at its receiving end**:

```
        module k                              module k+1
   ┌───────────────┐                       ┌───────────────┐
   │  U2.SER  ◄────┼───● J2.DATA ══════════┼──◄ QH  U1     │
   └───────────────┘   │                   └───────────────┘
                      ┌┴┐ R1 = 10k
                      └┬┘
                      ═╧═ GND
```

A fitted downstream module drives that net push-pull and swamps the 10 kΩ
completely. An absent one leaves it at a hard 0, which with the non-inverting
front end (HW-1) means **lamp off**. The same resistor sits at the master on its
`MISO` net, covering the case of no modules fitted at all.

The consequence is that `N` demotes from a correctness parameter to a performance
one — see [Module count](#module-count).

### Series resistors

| Ref | Value | Where | Why |
|---|---|---|---|
| `R2` | 33 Ω | in series with `U1.QH` before `J1.3` | Source-terminates the point-to-point `DATA` hop, and current-limits an output-to-output collision if a module is plugged in backwards. |
| `R3` | 33–100 Ω | in series with `CLK` **at the master only** | Damps the multi-drop clock bus. Source termination belongs at the driver, not at each receiver. |

`CLK INH` (pin 15) is tied **low** on every '165. Leaving it floating or high
stops that register shifting, which looks like eight repeated channels followed
by dead downstream modules.

### Use `QH`, not `/QH`

A '165 has complementary serial outputs — `QH` on pin 9 and `/QH` on pin 7, both
ordinary push-pull CMOS drivers. (There is no tri-state variant of this part, and
none is wanted: point-to-point push-pull is precisely why rev D needs no
arbitration.)

Take `QH`. Leave `/QH` **unconnected**.

`/QH` looks like a free hardware polarity inversion, but taking it would break
chain termination: `R1` pulls an absent module's `DATA` net *low*, and low must
mean *lamp off*. Inverting the serial output makes a missing or unpowered module
report every one of its channels as permanently **on**. If polarity ever needs
flipping, flip it in the front end (HW-1) or in firmware (`active_low`), where it
does not interact with the terminator.

## Frame structure

Each module consumes exactly **16 clocks**. There is no arm clock and nothing is
discarded — a '165 presents its first bit at `QH` the instant it is loaded,
before any clock arrives.

```
/PL  ──┐_____┌────────────────────────────────────────────┐_____┌──
       └ low ┘  ▲ all 16·N channels freeze on this edge    └ low ┘
                │
CLK  ═══════════╪═╗ ╔═╗ ╔═╗ ╔═╗       ╔═╗ ╔═╗ ╔═╗ ╔═══════════════
     (idles high)  ╚═╝ ╚═╝ ╚═╝  ...    ╚═╝ ╚═╝ ╚═╝
                   1   2   3           126 127 128

DATA  ─┤ ch0 │ ch1 │ ch2 │ ... ─────────────┤ ch126 │ ch127 ├──────
        └──── module 1 ────┘                └── module 8 ──┘
```

Total clocks = **16·N**, always a whole number of bytes (2 bytes per module). At
8 modules that is 128 bits / 16 bytes.

Stream bit *i* (0-based, MSB-first) is channel *i*. That is the whole mapping.

## Master protocol

1. Drive `/PL` low. All registers load transparently.
2. Raise `/PL`. **This edge is the sample instant.**
3. Issue **16·N clocks**, sampling `DATA` in the middle of each bit cell.
4. Drop `/PL` again. There is no minimum gap between frames beyond the '165's
   load pulse width (tens of nanoseconds).

### SPI mapping (FR-SCAN-7)

| SPI | Chain |
|---|---|
| `SCLK` | `CLK` |
| `MISO` | `DATA` |
| `MOSI` | unused (receive-only transaction) |
| `CS` | `/PL`, with **positive polarity** (`SPI_DEVICE_POSITIVE_CS`) |
| Mode | **2** (`CPOL=1, CPHA=0`) — idle high, sample on falling |

The `CS`-as-control trick carries over unchanged from rev C. With
`SPI_DEVICE_POSITIVE_CS` the line sits **low between transactions and high during
one**, which is exactly "load while idle, shift while transacting". The frame
reset is hardware-timed, cannot be forgotten or mistimed, and
`cs_ena_pretrans` sets the `/PL`-release-to-first-edge margin.

**Mode 2 is the one thing that changes from rev C.** A '165 updates `QH` on the
**rising** edge of `CLK`, so the master must sample on the **falling** edge to
land mid-cell:

```
              ┌───────┐       ┌───────┐       ┌───────┐
 SCLK  ───────┘       └───────┘       └───────┘       └───   (idles high)
              ▲       ▲       ▲       ▲
              │       │       │       │
     sample ──┘  shift┘ sample┘  shift┘
     (falling)  (rising)
```

Each bit is valid from one rising edge to the next, and the falling edge sits in
the middle of that window with roughly half a clock period of margin on both
sides. No inverter is needed anywhere in the clock path — unlike the rev C bench
rig, which needed a 74HC14 to escape an edge race.

> **Measured 2026-08-06/07** on a 4× 74HC165 rig (2 modules, 32 ch) at 4 MHz.
> `U1.D` read as channel 4 and `U2.D` as channel 12, exactly — a mode-3 sample
> on the shift edge would have slid the whole frame by one, so exact alignment
> settles CPHA as well as CPOL. Channel 0 was then read directly through input
> `H` (pin 6), which is the only channel whose capture timing differs: it is
> the bit `/PL` presents *before any clock arrives*, so reading it correctly is
> direct evidence that CPHA=0 catches the pre-clock bit rather than inference
> from frame alignment. See `BRINGUP.md` §7.

Bits always fill whole bytes, and with MSB-first ordering the unpack is:

```
channel i = (rx[i / 8] >> (7 - (i % 8))) & 1        // i < 16 * N
```

## Clock skew — why it stopped being a chain-length problem

Rev C inserted a NAND and a Schmitt inverter into the clock at every hop, so
module *k*'s clock lagged the master's by *k·d* and the far end eventually walked
out of its sample window. That failure scaled with chain length and was the
binding reason for HW-3.

Rev D busses the clock, so there is no per-hop gate delay at all — only flight
time down the harness, on the order of 0.5 ns per 100 mm hop. More usefully, the
remaining skew now works **in the design's favour**:

- `CLK` propagates master → downstream.
- `DATA` propagates downstream → master.

So module *k* is clocked *before* module *k+1*, and module *k* latches module
*k+1*'s `QH` value from **before** *k+1* had a chance to change it. Harness skew
therefore *adds* to the inter-device hold margin instead of eating it. The hold
budget is:

```
t_h(SER)  ≤  t_pd(CLK→QH)  +  skew(k → k+1)
```

With `t_h` ≈ 0–3 ns and `t_pd` ≈ 10–20 ns at 3.3 V, that holds with an order of
magnitude to spare before the skew term is even considered.

**Wire the `CLK` bus so it physically propagates along the harness** — daisy
through the connectors, module to module — rather than star-wiring it from the
master. Star wiring can put a downstream module *ahead* of an upstream one, which
subtracts from the hold margin instead of adding to it.

What replaces skew as the clock-rate limit:

1. The '165's own `fmax` (roughly 25 MHz for HC at 3.3 V; higher for LV/LVC).
2. **Signal integrity on a multi-drop clock.** This is a genuine regression from
   rev C, where `CLK` was point-to-point and re-driven at every hop. Rev D hangs
   up to 16 register inputs plus 8 connector stubs on one net. Mitigate with `R3`
   at the master, short stubs from connector to IC, and by not reaching for clock
   rate — 4 MHz already meets the 10 kHz target at 128 channels with margin
   (`TIMING.md` §2.4).

## Module count

`N` (`PINLED_NUM_MODULES`) is still configuration, but a wrong value is now
benign in both directions:

| Setting | Result |
|---|---|
| `N` **too high** | Trailing bits read a hard 0 from the local pull-down → those channels report "lamp off" permanently. The only cost is frame time spent on channels that do not exist. |
| `N` **too low** | Trailing modules are never clocked; their channels do not exist as far as firmware is concerned. Their registers simply hold. |

Neither case shifts or misaligns the channels that *are* present, because the
bit-to-channel mapping is a fixed stride of 16 with no arm clocks to stay in
phase with. Setting `N` = 8 on a two-module bench rig is a perfectly valid way to
work; it costs 49 µs per frame instead of 25 µs.

There is still no reliable way to **auto-detect** the true count. An all-zeros
tail is indistinguishable from a fitted module whose lamps are all off, and dumb
shift registers cannot announce themselves without a third IC per module carrying
a presence bit — which is not worth it given how harmless a wrong count is.

## Failure modes

| Fault | Symptom | Notes |
|---|---|---|
| Module *k* absent or unpowered | channels ≥ 16(*k*−1) all read 0; modules nearer the master unaffected | The pull-down defines the level, so this is a clean, stable zero rather than noise. Same class of failure as rev C's clock token dying mid-chain. |
| Wrong SPI mode | **every channel reads its neighbour** — a clean one-bit shift of the whole frame | Distinctive: stable, not noisy, and channel 0 becomes unreachable. Try the other mode before suspecting hardware. |
| `/PL` stuck **low** | every bit in the frame identical (channel 0 of module 1, repeated) | Registers stay transparent and never shift. |
| `/PL` stuck **high** | first frame plausible, then all zeros forever | Registers never reload; the terminator's zeros shift in and stay. |
| `CLK INH` floating or high on one '165 | that register's 8 channels repeat, everything downstream of it dead | Easy to miss — check pin 15 first when 8 channels misbehave as a block. |
| Module plugged in backwards | two `QH` outputs shorted; `DATA` reads garbage | `R2` limits the fault current. Key the connectors. |
| `N` too high / too low | see [Module count](#module-count) | Benign either way |
| Clock too fast for the harness | intermittent, *noisy* errors | Unlike rev C, this does **not** present as a clean one-channel shift — that signature now means the SPI mode, not the clock rate. |

## Per-module bill of materials

| Ref | Part | Qty | Function |
|---|---|---|---|
| U1 | 74LVC165 | 1 | channels 0–7; `QH` → `J1.3` via `R2`, `SER` ← `U2.QH` |
| U2 | 74LVC165 | 1 | channels 8–15; `SER` ← `J2.3` |
| U3 | 3.3 V LDO | 1 | local regulation from the 5 V rail (HW-8) |
| R1 | 10 kΩ | 1 | pull-down on `J2.3` — self-terminates the chain |
| R2 | 33 Ω | 1 | series with `U1.QH`; source termination + collision limit |
| C1, C2 | 100 nF | 2 | one per logic IC (HW-5) |
| C3 | 10 µF | 1 | bulk decoupling |
| C4, C5 | 1 µF | 2 | LDO input / output |
| J1, J2 | 5-pin JST-SH | 2 | chain in (toward master) / chain out |

**Two logic ICs per module**, down from six.

Front end (16 × MOSFET + inverting Schmitt, HW-1) drives `LINE0..LINE15` and
lives on its own sheet — it is unchanged by rev D and still needs 3× '14 per
module for the Schmitt sections.

At the **master**, once per chain: `R3` (33–100 Ω series on `CLK`) and a 10 kΩ
pull-down on `MISO` (HW-2, HW-11).

> **Family:** LVC is preferred (HW-3) for edge rate and drive into a multi-drop
> clock bus. `74LV165A` is an acceptable substitute. `74HC165` is the right
> choice for a **DIP breadboard bench build**, where LVC is not made in a
> through-hole package.
>
> The ≤ 2 MHz figure for HC was conservative. **4 MHz ran clean on a 4-chip
> breadboard rig** (2 modules, 32 ch) at 3.3 V for millions of frames with zero
> read errors, 2026-08-06/07. That does not license 4 MHz at eight modules —
> what fails first is multi-drop `CLK` integrity across 800 mm (§4.3), which is
> a harness-length problem rather than a part-speed one, and is untested.
>
> Pin numbers: most are now confirmed against silicon (see `HARDWARE.md`), but
> pins 3, 4, 5 and 13 are not. Confirm the exact part's pinout against its
> datasheet before layout.

## Net list

Every connection, for building or for checking a schematic.

| Net | Members |
|---|---|
| `VCC5` | J1.1, J2.1, U3.VIN, C4 |
| `V3V3` | U3.VOUT, C5, U1.16, U2.16, C1, C2, C3 |
| `GND` | J1.2, J2.2, U3.GND, U1.8, U2.8, U1.15, U2.15, R1.2, all caps |
| `CLK` | J1.4, J2.4, U1.2, U2.2 |
| `PL_N` | J1.5, J2.5, U1.1, U2.1 |
| `DATA_UP` | J1.3, R2.2 |
| `QH1` | U1.9, R2.1 |
| `LINK` | U1.10 (`SER`), U2.9 (`QH`) |
| `DATA_DN` | J2.3, U2.10 (`SER`), R1.1 |
| `LINE0..7` | U1 pins 6,5,4,3,14,13,12,11 (from the front-end sheet) |
| `LINE8..15` | U2 pins 6,5,4,3,14,13,12,11 (from the front-end sheet) |

Tied low: `CLK INH` (pin 15) on both registers. Nothing is tied high.
Left unconnected: `/QH` (pin 7) on both registers — see
[Use `QH`, not `/QH`](#use-qh-not-qh).

Note that `U1.15`/`U2.15` appear in `GND` — that is `CLK INH`, not a supply pin.
The '165's ground is pin 8 and its supply is pin 16.

## Artifacts

| File | What |
|---|---|
| `chain_timing.svg` | Rev D waveforms: load, snapshot edge, shift, mode 2 sampling |
| `pinled_module_revC.kicad_sch` | **Superseded.** Rev C schematic, retained for reference only — rev D was bench-validated 2026-08-06 (`BRINGUP.md` §7) and this can be dropped once a rev D schematic exists. |

No rev D schematic exists yet. The net list above is the authority; it is short
enough to wire from directly, which is the intent for the bench build — and it
has now been wired from directly, on a 4× 74HC165 rig, without correction.
