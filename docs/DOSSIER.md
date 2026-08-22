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
 (5–20 V, AC/DC) │   FET level-shift + protection      74LVC165 U1  (ch 0–7)                    │
   L0 ───────────┼──▶ [divider · diode · clamp] ──────▶ H,G,F,E,D,C,B,A                         │
   ...           │      + inverting Schmitt                    │  QH ─33R─▶ J1.DATA ──▶ master  │
   L15 ──────────┼──▶ [divider · diode · clamp] ──────▶ H..A    │                               │
                 │   74LVC165 U2  (ch 8–15)  QH ──▶ U1.SER ────┘                                │
                 │                           SER ◀── J2.DATA ◀── downstream module's QH         │
                 │                                   └─ 10k pull-down (self-terminates)         │
                 │                                                                              │
                 │   /PL  ─── bussed ──▶ pin 1 of BOTH registers  (low = load, high = shift)    │
                 │   CLK  ─── bussed ──▶ pin 2 of BOTH registers  (shifts on RISING edge)       │
                 │   CLK INH (pin 15) tied LOW      /QH (pin 7) left unconnected                │
                 │   3V3 LDO: local regulation from the 5 V harness rail                        │
                 └──────────────────────────────────────────────────────────────────────────────┘

ESP32-S3 ── SPI + DMA ──▶ SCLK = CLK,  MISO = DATA,  CS = /PL   (one transaction = one frame)
                          mode 2 (CPOL=1 CPHA=0) · 16·N bits · nothing discarded
ESP32-S3 ── 1 GPIO ─────▶ WS2812B / SK6812 string (RMT)   ← firmware maps sensed channel → LED
```

**Two logic ICs per module.** Rev C needed six ('161 counter, two '251 muxes,
'109 dual flip-flop, '10 NAND, '14 Schmitt) to build a distributed state machine
whose only job was taking turns on a shared bus. A shift register chain *is*
taking turns, so the state machine disappears.

**The ESP32 spends 3 GPIO on sensing, total, regardless of module count:** `CLK`,
`DATA` and `/PL` — all three from a single SPI peripheral — plus **one** GPIO
for the entire LED string. There is no per-module data line.

### Why a shift register and not a counter + mux

Rev B and rev C both scanned by *addressing*: a '161 counter walked an address
onto tri-state '251 muxes, and extra logic decided which module was allowed to
drive the shared `DATA` line. That is where the '151-vs-'251 argument came from —
a '151's outputs are push-pull and cannot share a line, so the tri-state '251 was
mandatory.

Rev D scans by *shifting*, and nothing shares a line, so that whole argument
retires. A '165's `QH` is an ordinary push-pull output driving exactly one
receiver, one hop away. There is no tri-state variant of the '165 and none is
wanted.

What the change bought:

| | rev C | rev D |
|---|---|---|
| Logic ICs / module | 6 | **2** |
| Clocks / module | 17 (one spent arming) | **16** |
| `DATA` | shared, arbitrated, tri-state | point-to-point, push-pull |
| Capture | swept across the frame | **one instant, whole chain** |
| Clock | regenerated per hop, skew accumulates | bussed, skew *adds* hold margin |
| Missing modules | read the master's bias | read a local hard 0 |

What it cost: `CLK` became multi-drop instead of point-to-point (`TIMING.md`
§4.3), and `J1`/`J2` are no longer interchangeable — a module fitted backwards
ties two push-pull outputs together, so the connectors must be keyed (HW-13).

### Daisy-chaining / expansion (16 channels per module)

Modules are **identical, unaddressed, and chained on 5 conductors** — `VCC`,
`GND`, `DATA`, `CLK`, `/PL`. `CLK` and `/PL` are bussed to every module; `DATA`
chains `QH` → `SER`, so the whole harness behaves as one shift register 16·N
bits deep.

A frame is three steps: drop `/PL` (every register transparently follows its
inputs), raise it (**every channel in the chain freezes on that one edge**),
then clock 16·N bits out in a single SPI transaction. No addressing, no
arbitration, no arm clocks, nothing discarded.

This replaces four earlier proposals. The **star** topology (one `DATA_IN` GPIO
per module) cost a pin per module and did not scale. The **strapped-ID** variant
(cascaded counters comparing a wrap count against a jumper) made physical order
depend on correct strapping. The **4-wire RC** variant put an analog time
constant in the middle of the arbitration, capped the frame rate with a
mandatory dead gap, and gave the chain a few-microsecond stall cliff. **Rev C**
fixed all of that with a fifth conductor and a `STARTED` flip-flop — but still
spent six ICs per module maintaining a distributed state machine.

A fifth was evaluated and rejected (2026-08-04): **8× MCP23S17 SPI I/O
expanders** on a shared bus. One IC per 16 channels is fewer parts still, and
its wire-time arithmetic was sound, but four things decided against it:

1. **Eight transactions per frame.** The MCP requires `CS` to toggle between
   devices, so 128 channels cannot be one transaction. Against the measured
   ~17 µs of fixed per-transaction overhead that is 8 × (17 + 3.2) ≈ **162 µs**
   a frame — a 6.2 kHz free-run the boot check clamps to ~3.7 kHz, missing the
   10 kHz target by 2.7×. A '165 chain does 128 channels in one 49 µs
   transaction.
2. **The harness grows.** The MCP needs `MOSI` too, so the inter-module
   connector goes from 5 conductors to 6 or 7, bussing *four* fast signals with
   eight stubs where rev D busses one.
3. **`HAEN=0` is the power-on default**, and a device in that state ignores its
   address pins and drives `SO` on every read — so one browned-out or
   hot-plugged module corrupts **all 128 channels**, not just its own 16.
4. **Sample skew.** Read serially, channel 0 and channel 127 land ~160 µs
   apart. Rev C's spread was 35 µs; rev D captures every channel on one edge.

> **One thing could still revive it.** Queueing all eight reads with
> `spi_device_queue_trans()` so the driver's ISR chains them back-to-back may
> amortise most of the ~17 µs. That has never been measured. If it works, one
> IC per 16 channels deserves a second look — reason 1 is the only one of the
> four that this would answer, but it is the one that decided the question.

The trade rev D keeps: a module that is missing or unpowered blanks everything
downstream of it, where the star topology would have lost only that module's
channels. It fails cleanly, though — the receiving pull-down holds a hard zero,
and modules nearer the master are unaffected.

Full protocol and failure analysis in `CHAINING.md`.

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
chop frequency — target **a few kHz per channel**; the design settles on a fixed
**10 kHz** (FR-SCAN-5). At 128 channels that is 1.28 M mux steps per second,
which is why the chain is clocked by SPI + DMA at 4 MHz rather than bit-banged.
Since rev D every channel is captured on a single `/PL` edge, so sequential-scan
phase skew is gone entirely rather than merely washing out in the integrator.

The measured ceiling is ~20 kHz free-run at 128 channels — 49 µs a frame, of
which ~17 µs is fixed SPI driver overhead and 32 µs is the burst. The boot check
clamps to 60% of that, so **Fs ≈ 12 kHz**. See `TIMING.md` §2.4.

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
| U1, U2 | 74LVC165 | 2 | 8-bit parallel-in / serial-out shift register. `U1` = ch 0–7 (nearest master), `U2` = ch 8–15 → `U1.SER`. Channel 0 wires to input `H`. `CLK INH` low, `/QH` unconnected. |
| R1 | 10 kΩ | 1 | pull-down on `J2.DATA` — self-terminates the chain (HW-11). |
| R2 | 33 Ω | 1 | series with `U1.QH`; source termination + collision limit if a module is reversed. |
| J1, J2 | 5-pin JST-SH | 2 | chain in (toward master) / chain out. **Not interchangeable** (HW-13). |
| Q1–Q16 | N-ch MOSFET (e.g. 2N7002 / BSS138) | 16 | per-channel level-shift 5–20 V → 3.3 V logic (inverting common-source). |
| D1–D16 | signal diode (e.g. 1N4148 / BAT54 Schottky) | 16 | AC-signal rectification / input steering. |
| — | gate series R + 12 V G–S zener, drain pull-up | — | protection + defined trip point with hysteresis. |
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
- **Protection:** series gate resistors + 12 V gate–source zener per channel
  (V_GS max ±20 V vs the ~24 V input span; see HARDWARE.md). No output clamp
  (Schottky or the input's own protection diodes via series R, or a TVS). The
  machine is electrically noisy — the machine's own filtering helps, but don't
  rely on it exceeding the HC absolute-max of VCC + 0.5 V.

---

## 7. Assumptions & open questions

Stated so the first cut can proceed; flag any to change:

1. **Topology:** two chained `74x165` shift registers, 16 ch/module, one SPI
   transaction per frame (rev D). Supersedes the counter+mux topology of rev B/C.
   **Bench-validated 2026-08-06/07** on 4× 74HC165 (2 modules, 32 ch) at
   4 MHz: SPI mode 2, `H`-first bit order, the `QH`→`SER` handoff, HW-11
   self-termination and hot-plug recovery all measured, plus channel 0 read
   directly via input `H`. Most '165 pin numbers are now confirmed against
   silicon; pins 3, 4, 5 and 13 remain unexercised — see `HARDWARE.md`.
2. **MCU:** original ESP32 (QT Py ESP32 Pico), ESP-IDF **5.5.x**, matching the
   POC's `CMakePresets`. S3 is a drop-in later (more RAM/RMT channels).
3. **LED driver:** `zorxx/neopixel` (RMT) as in the POC; `espressif/led_strip`
   is a documented alternative.
4. **Sensing point:** at/near each **socket** (one tap = one lamp = one LED),
   so no matrix row/column decoding is required.

   **The original bulb comes out** — replacing it is the entire point. That is
   settled, not open, and it makes the module's front end responsible for two
   electrical jobs the incandescent used to do for free. Both are handled with
   **jumper-selectable power resistors** ("phantom loads") beside the sense tap:

   1. **SCR holding current.** Bally/Stern-era drivers latch lamps with SCRs,
      which conduct only while their load draws holding current. A ~250 mA bulb
      supplied it; a sense tap does not, so the lamp never properly latches on.
      The known field fix is ~470 Ω across the socket, ~13 mA at 6.3 VAC.
   2. **Supply regulation.** The lamp rails are **unregulated** and specified
      under load. Removing most of a machine's load lets the rail climb out of
      spec — a whole-system effect that does not present as "this lamp
      flickers," which is what makes it easy to miss until it is bafflingly
      wrong everywhere at once.

   Jumpers because the requirement is per-machine, not universal: a
   transistor-driven game on a regulated supply wants no phantom load, and
   paying for it there is waste heat. The front end therefore also
   **half-wave rectifies** the AC tap and level-shifts to 3.3 V logic — see
   `HARDWARE.md` "Front-end (per channel) checklist".

   > **Consequence for the power budget.** `TIMING.md` §5.1's 3 mA/channel
   > placeholder describes the FET path only. Where phantom loads are fitted
   > they dominate it by roughly 4×, and they dissipate on the *lamp* rail
   > rather than the 3.3 V rail, so they do not belong in the same total.
   > Neither number is measured yet.
5. **First target: Bally/Stern solid-state, 1977–1985** *(fixed 2026-08-07)*.
   The bench target is an **Alltek Ultimate MPU** — a universal replacement for
   Bally `AS2518-17/-35/-133`, `AS-2517-35` and Stern `MPU-100`/`MPU-200` — with
   the **Alltek Ultimate Test Card** on `J2`. Selecting *Enhanced Diagnostics
   Mode* on the game-select dipswitch lets the card fire **individual lamps, or
   whole rows and columns**, plus single solenoids and arbitrary display digits.

   This is a better validation target than one machine: the MPU carries **90+
   Bally/Stern titles** in EPROM, dipswitch-selectable, so one bench setup
   reproduces the lamp behaviour of the whole era rather than a single game's.
   The test card also gives *deterministic* lamp patterns on demand, which is
   what a classifier needs — real gameplay is not reproducible.

   > **The drive waveform is not yet measured, and it matters more than usual.**
   > Bally/Stern of this era does not use a Williams-style scanned lamp matrix.
   > It latches lamps with **SCRs on an AC supply**, so a lit lamp sees
   > phase-related AC rather than a few-hundred-Hz low-duty burst. Scope one
   > lamp on this rig before trusting any number here: supply voltage, whether
   > conduction is half- or full-wave, the envelope rate (50/60 Hz vs 100/120 Hz)
   > and the duty of a lamp the game is *dimming* rather than holding on.
   > Everything the profiler thresholds on comes from that measurement.

   **Consequence for §6's classifier:** this target exercises `AC_STEADY` and
   `AC_DIMMED`, and **not** `MATRIX`. The matrix path — the one the filament
   argument leads with — will stay unvalidated on hardware until a scanned-matrix
   machine (Williams System 11, Data East, later Sterns) joins the bench. Until
   then it rests on host unit tests with synthetic waveforms alone.
