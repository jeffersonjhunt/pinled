# Bring-up Guide — `pinled` v2

How to take a scan module from "just wired" to "verified", and the traps that
make a working board look broken. Companion to `HARDWARE.md` (what to build) and
`TIMING.md` (why the numbers are what they are).

The firmware ships three diagnostics (FR-DIAG-1..4), all off or normal by
default. They form a ladder: each one answers a question the previous one
raised, and they take **increasing** ownership of the scan hardware.

| Kconfig | Default | Answers |
|---|---|---|
| `PINLED_SCAN_DEBUG` | `y` | Is any channel responding at all? |
| `PINLED_SCAN_STEP_MS` | `0` (off) | Is the chain actually shifting? |
| `PINLED_SCAN_HOLD_CH` | `-1` (off) | Where exactly does the signal die? |
| `PINLED_SPI_SWEEP` | `n` (off) | How fast can the chain be clocked? |

`SCAN_HOLD_CH` takes precedence over `SCAN_STEP_MS`. Either one suppresses
`scan_task` — they own the chain and would otherwise race it — so **the LED
string does not update while either is active.** The log says so at startup.

> All four work on **production** hardware. The chain holds no analog state, so
> it can be clocked arbitrarily slowly or stopped outright. (Under the rev B
> 4-wire design the two slow modes would have been bench-rig only — stopping the
> clock there released the bus grant.)

## 1. `PINLED_SCAN_DEBUG` — is anything alive?

Logs the raw pre-filament frame twice a second:

```
I (2618) pinled-main: raw [00001111]  tog [----TTTT]
```

- **`raw`** — the chain right now, one character per channel, grouped in eights.
  Group boundaries are shift registers: left is `U1` (ch 0–7), right is `U2`
  (ch 8–15).
- **`tog`** — sticky since boot. `T` means that channel has been observed *both*
  high and low, so it is genuinely switching rather than stuck.

`tog` being sticky is the point: you can press test inputs at your leisure and
read the result later, with no timing to coordinate.

A channel showing `-` forever is unconnected, unpopulated, or pinned by the
chain terminator. It is never merely "idle" — an idle lamp still reads a stable 0, and that
low is recorded on the very first frame.

This isolates the sense bus from both the filament model and the LED string, so
a dead strip and a dead bus stop looking identical.

## 2. `PINLED_SCAN_STEP_MS` — is the chain shifting?

At full speed the scan runs tens of thousands of frames per second, so `CLK`
toggles in the megahertz. **Indicator LEDs on the chain signals cannot show
this** — they sit at a steady half-brightness, which reads as "not running" and
sends you hunting a fault that isn't there.

Set it to `250` and the scan advances one bit every 250 ms:

```
I (127)  --- pass start: /PL re-asserted before every step ---
I (377)  bit   0  -> module 0 ch  0  ('165 U1 pin H)  DATA=0
I (627)  bit   1  -> module 0 ch  1  ('165 U1 pin G)  DATA=1
```

A 16-channel module takes 4 s per frame. Unlike rev C there is no counter to
watch on a scope — the only observable is `DATA` itself, which is why the log
prints the pin each bit came from.

**The mode matters here.** The walk re-asserts `/PL` between steps (FR-DIAG-2)
so a button pressed mid-walk is still visible. If it did not, you would be
walking one frozen snapshot and every press during the walk would be invisible —
a real behavioural difference from rev C, where the mux read live.

If the chain does **not** shift, check in this order:

1. **`CLK INH` (pin 15) tied low** on every '165. This is the number-one rev D
   wiring fault: floating, it stops that register dead while its neighbours keep
   going, so you get eight repeated channels and nothing downstream.
2. **`/PL` actually rising.** Held low, the registers stay transparent and never
   shift — `DATA` reads channel 0 of module 1 forever.
3. **The SPI mode.** Mode 3 instead of 2 shifts everything by one bit and makes
   channel 0 unreachable. See §4.

## 3. `PINLED_SCAN_HOLD_CH` — where does the signal die?

The walk sweeps past any given channel for only a few hundred ms per cycle,
which is useless for tracing with a meter. Hold snapshots the chain, clocks
exactly *k* times and stops, so `DATA` becomes a static DC level:

```
I (127) chain parked on channel 4 (module 0, U1 input D, pin 14); 4 clocks issued
I (627) hold ch 4  DATA=1
```

Measure along the path and find the first link that stops tracking:

| Node | Should read |
|---|---|
| front-end output at the '165 input pin | your input — follow it with the button |
| `U1.QH` (pin 9) | equal to that input, for the parked channel |
| after `R2` (33 Ω) | the same |
| MCU `DATA_IN` | the same |

**The more useful variant is `/PL` held low**, which is what
`PINLED_SCAN_HOLD_CH=0` does rather than clocking. The registers stay
transparent, so `U1.QH` continuously tracks module 1 channel 0 — press the
button and watch the meter move in real time. Rev C had no equivalent; its mux
only presented a channel while the counter addressed it.

For a chained rig, park on a channel in the *far* module. If near modules track
and far ones do not, the fault is between them: a missing `QH` → `SER` link, a
`CLK INH` left floating, or a module fitted backwards.

## 4. Interpreting a full-speed frame

With the scan running normally, these signatures come up repeatedly:

| Symptom | Cause |
|---|---|
| **Every channel reads its neighbour — stable, not noisy** | **Wrong SPI mode.** Try 2 ↔ 3 before suspecting anything else. Mode 3 samples on the same edge the '165 shifts on, so the whole frame slides one bit and channel 0 becomes unreachable. This is the single most likely rev D bring-up fault. |
| All 16 bits identical | `/PL` stuck low — registers transparent, never shifting. Everything reads module 1 channel 0. |
| Plausible first frame, then all zeros forever | `/PL` stuck high — registers never reload, so the terminator's zeros shift in and stay. |
| Eight channels repeat, everything past them dead | `CLK INH` (pin 15) floating or high on one '165 |
| Bits 0–7 work, 8–15 never | `U2.QH` → `U1.SER` link missing |
| Everything reads 1 | `/QH` (pin 7) wired instead of `QH` (pin 9), or the terminator/internal pull is backwards |
| Everything reads 0 | `DATA` not connected, or `/PL` never rising |
| Channel order reversed within each byte | channel 0 wired to input `A` instead of `H` — electrically fine, but the driver expects `H` first |

On a **chained** rig, add these:

| Symptom | Cause |
|---|---|
| All channels beyond 16·k read 0, near modules fine | module k+1 missing, unpowered, or its `QH` → `SER` link is open. The terminator holds a clean zero, so this is stable rather than noisy. |
| `DATA` garbage on a specific module boundary | that module fitted backwards — two `QH` outputs shorted (HW-13) |
| Far modules intermittently wrong, worse at higher clock | multi-drop `CLK` integrity — the rev D binding constraint. Fit/raise the series resistor at the master, or drop `PINLED_SPI_HZ`. |

> **The off-by-one signature changed meaning between revisions.** On rev C
> hardware, a clean whole-frame one-channel shift meant the *clock* was above the
> ceiling. On rev D it means the *SPI mode*. Exceeding the clock ceiling on a
> '165 chain should be noisy and intermittent, because what fails first is
> multi-drop signal integrity rather than a deterministic sampling window
> (`TIMING.md` §4.3).

## 4b. `PINLED_SPI_SWEEP` — how fast can it go?

Steps the chain clock through a ladder from 1 to 40 MHz, and at each rate reads
256 frames and reports how many agreed plus the union of every bit seen. A pass
takes a few milliseconds and repeats every 3 s, so there is no timing to
coordinate — hold one input down and read the last pass.

```
  8000000 Hz (actual  8000000): stable 256/256  [00001000]  union=0x10
 20000000 Hz (actual 20000000): stable 256/256  [00000100]  union=0x20
 40000000 Hz (actual 40000000): stable  32/256  [00000000]  union=0x40
```

A rate is good when it is `256/256` **and** the pattern shows the channel you
are actually holding. Both conditions matter: above the ceiling the reads stay
fully stable and simply shift by one, so stability alone will happily certify a
broken rate.

## 5. Building the rev D bench rig

Two '165s prove one module; **four prove the design**, because the open question
is the chain handoff and the terminator, not the register. Build two modules.

### Shopping list

| Qty | Part | Notes |
|---:|---|---|
| 6 | **74HC165N**, DIP-16 | 4 needed, 2 spare. HC not LVC: LVC is not made in through-hole. Run at **3.3 V** and hold the clock to ≤ 2 MHz (HW-3). |
| 4 | 10 kΩ resistor | chain terminators — one per `SER` input that faces a connector, plus one at the MCU |
| 4 | 33 Ω resistor | series with each `QH` |
| 1 | 100 Ω resistor | series with `CLK` at the MCU |
| 6 | 100 nF ceramic | one per IC, close to pin 16 |
| 2 | 8-way DIP switch | test inputs, 8 per module |
| 2 | 10 kΩ ×8 resistor network (SIP-9 bussed) | pull-downs for the DIP switches — 16 discrete resistors also works, just tedious |
| — | breadboard + jumpers | you have these |

Everything else is on hand: the ESP32-S3-DevKitC-1 and the LED strip.

> **3.3 V, not 5 V.** GPIO 9 is not 5 V tolerant. A '165 on a 5 V rail drives
> 5 V into the S3.

### Wiring, one module

```
  DevKitC-1                    U1 (ch 0–7)              U2 (ch 8–15)
  IO18 SCLK ──100R──┬─────────▶ pin 2  CLK    ┬────────▶ pin 2  CLK
  IO17 CS//PL ──────┼─────────▶ pin 1  /PL    ┼────────▶ pin 1  /PL
  IO9  MISO ◀──33R──┴ pin 9 QH                │
                    │                          │
                   10k                         │
                    ▼                     pin 9 QH ─────▶ pin 10 SER  (of U1)
                   GND
                                          pin 10 SER ◀── next module's QH,
                                                         or 10k to GND if last
  3V3  ──▶ pin 16 on both        GND ──▶ pin 8 AND pin 15 on both
```

`pin 15` is `CLK INH` and must be **grounded**, not left floating — it sits next
to `VCC` and is the easiest pin on the part to get wrong.

Test inputs, module-local channel order:

| Channel | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| '165 input | `H` | `G` | `F` | `E` | `D` | `C` | `B` | `A` |
| Pin | 6 | 5 | 4 | 3 | 14 | 13 | 12 | 11 |

Every input needs a defined level — nothing may float. Two ways, and the
2026-08 rig used both:

- **Switched:** a button to 3V3 plus a 10 kΩ pull-down. Needed only on inputs
  you intend to exercise.
- **Tied off:** a direct wire to `GND` for the rest. Cheaper than a resistor
  per channel and a harder zero, but that channel can then never be tested.

The 2026-08 rig had **four buttons total** — the `D` input (pin 14) on each of
the four chips, i.e. channels 4, 12, 20 and 28 — with all 28 other inputs wired
straight to ground. Every button registered on its expected channel.

> **Partly confirmed against silicon, 2026-08-06/07.** Working end to end on a
> 4× 74HC165 rig proves pins 1 (`/PL`), 2 (`CLK`), 8 (`GND`), 9 (`QH`),
> 10 (`SER`), 15 (`CLK INH`) and 16 (`VCC`). Pin 14 (`D`) was confirmed by
> button press on all four chips, and **pin 6 (`H`, channel 0) by a button
> moved onto it** — `raw [10000000 …]`, `tog [T------- …]`, exactly one
> channel and no neighbours. Pins 11/12 (`A`/`B`) are confirmed only by their
> floating-input signature landing on local channels 7/6 before they were tied
> off, which is weaker evidence than a press.
>
> Pin 6 was worth the trouble specifically because channel 0 is the only
> channel whose capture timing differs — it is the bit `/PL` presents before
> any clock arrives. Reading it correctly is direct evidence that CPHA=0
> catches the pre-clock bit; every other channel can only establish that by
> inference from frame alignment.
>
> **Pins 3, 4, 5 and 13 (`E`, `F`, `G`, `C`) remain unexercised, and the rig
> as wired cannot exercise them** — they are hard-grounded, so they read 0
> permanently and produce no evidence either way. Confirming them needs a
> button moved or a ground lifted.

> **Do not trust flags set while wiring is being handled live.** A first
> attempt at the pin-6 test appeared to flag channels 0–3 together. Those are
> pins 6, 5, 4 and 3 — four *physically adjacent* pins on the DIP — which is
> the signature of a lead brushing neighbours, not of three inputs
> independently losing ground. Re-run after a reset with hands clear, it was a
> single clean channel. `tog` is sticky, so one transient during handling is
> indistinguishable from a real press forever after.
>
> Note also that **attaching the logger resets the board** and wipes the
> flags. Start the logger *first*, then press.

### Firmware settings

```
PINLED_SPI_MODE=2          # the thing being tested; the default
PINLED_SPI_HZ=2000000      # HC parts on a breadboard
PINLED_PL_FROM_CS=y        # CS drives /PL, positive polarity
PINLED_NUM_MODULES=1       # then 2
PINLED_CHANNELS_PER_MODULE=16
PINLED_ACTIVE_LOW=n
PINLED_SCAN_DEBUG=y
```

Only `SPI_HZ` and `NUM_MODULES` differ from the shipped defaults. `PINLED_MR_*`
and `PINLED_ARM_CLOCK` no longer exist — they were replaced by `PINLED_PL_*` and
deleted respectively when the firmware moved to rev D framing.

### What to check, in order

1. ~~**Mode 2.**~~ **PASSED 2026-08-06.** `U1.D` read as channel 4 and `U2.D`
   as channel 12, exactly, at 4 MHz on 4× 74HC165. Re-confirmed 2026-08-07 on a
   QT Py ESP32-S3 (FH4R2).
2. ~~**Bit order.**~~ **PASSED** — same run. Channels ascend with no byte
   reversal, channel 0 on pin `H`. The exact landing of `D` on channel 4 is
   itself the proof: a dropped first bit would slide the whole frame.
3. ~~**Second module.**~~ **PASSED** — the rig is 2 modules / 32 channels.
   Channels 16–31 appear in order with no gap, so the `QH`→`SER` handoff works
   across a module boundary as well as within one.
4. **`/PL` held low.** `DATA` should track channel 0 live on a meter. *Still
   open* — needs a `PINLED_SCAN_HOLD_CH=0` build, which the bench has not run.
5. ~~**Unplug the second module** while running.~~ **PASSED 2026-08-07.**
   Chips 3–4 pulled live; channels 16–31 held a hard zero across **~4.9 M
   frames** with not one toggle flag set. The positive control matters as much
   as the silence: with the module out, `U1.D` still read channel 4 and `U2.D`
   channel 12, proving the frame was still 32 bits and those trailing bits were
   genuinely being clocked from a pulled-down `SER` rather than from a dead
   chain. Groups 3–4 also stayed clean *during* those presses, so the 10 kΩ
   terminator holds against crosstalk on the shared `CLK`//`PL` bus, not merely
   against quiescence. HW-11 confirmed: `num_modules` is a performance setting,
   not a correctness one.

### What does *not* transfer from the rev C rig

The '161 + '151 breadboard validated the SPI plumbing — peripheral setup,
GPIO-matrix routing, receive-only transactions, MSB-first unpack, `CS` as a
control line. All of that carries over. **Nothing about the sampling phase
does**: that rig needed a 74HC14 in the `CLK` path to make its counter advance
on the falling edge, and rev D needs no inverter anywhere because the phase is
handled by the SPI mode instead.

## 6. Tooling traps

These are not hardware faults, but they present as one.

**Opening the serial port resets the board.** On the DevKitC-1's native `USB`
port, RTS drives `EN`. `cat /dev/ttyACM0`, `idf.py monitor`, and pyserial all
trigger it — which silently wipes the sticky `tog` flags, so a working board
reports nothing. Keep **one** long-lived reader attached and read its output
file instead of reopening the port.

**ROM download mode looks exactly like a hang.** In download mode the board
still enumerates as `303a:1001` and prints its banner once, then goes quiet —
indistinguishable from firmware that never started. Check the `boot:` field:

```
rst:0x15 (USB_UART_CHIP_RESET),boot:0x0 (DOWNLOAD(USB/UART0))   <- download
rst:0x1  (POWERON),            boot:0x8 (SPI_FAST_FLASH_BOOT)   <- running
```

A latched force-download flag survives `EN` resets; only a **USB replug** clears
it. A held or stuck `BOOT` button produces the same `boot:0x0`.

**A capture that has stopped looks exactly like a hardware regression.** The
serial logger originally had a fixed lifetime, and once expired a few hundred
milliseconds before a round of button presses. Its tail showed all-zeros and no
toggles — indistinguishable from a dead scan — and that was very nearly recorded
as a fault in a working design. The logger now runs until killed. Check
`pgrep -c -x -f "python /tmp/pinled_logger.py"` **before** reading its output,
and sanity-check the last timestamp against how long the board has been up.

When the board is driving LEDs, believe the LEDs: they are the more direct
evidence, and a stale text log is not.

**New Kconfig symbols need a reconfigure.** Adding an option does not add it to
an existing `sdkconfig`, so the `#ifdef` compiles the feature out and the build
appears to succeed with nothing changed. Delete `sdkconfig` and rebuild.

**`idf.py` is a shell function**, so `timeout idf.py ...` fails with "No such
file or directory". Call `python $IDF_PATH/tools/idf.py` instead. `idf.py
monitor` additionally requires stdin to be a TTY.

## 7. Status

### Carried over from the rev C bench rig

Verified on an ESP32-S3-DevKitC-1 (`N8R8`) against a breadboard module built
around a 74x161 + 74x151. **Only the first group still applies to rev D.**

Still valid — SPI plumbing, hardware-independent:

- **SPI + DMA scan path**: peripheral config, GPIO-matrix routing of
  `SCLK`/`MISO`, receive-only transaction, MSB-first unpack, and actual-vs-
  requested clock rate (note `spi_device_get_actual_freq()` reports **kHz**).
- **`CS` as a control line** with `SPI_DEVICE_POSITIVE_CS`: low between
  transactions, high during. Rev C used it for `/MR`; rev D uses the identical
  mechanism for `/PL`.
- **10 kHz pacing** via `gptimer` + task notification, zero overruns across
  ~6.1 M frames.
- **Polarity** non-inverting: input high → logic 1, `active_low = n` (HW-1).
- **One strip transmit per LED frame** (FR-LED-6) and the boot refresh clamp
  (FR-LED-8).

Superseded by rev D — do not carry these forward:

- Counter/address-decode results, `/MR` clearing, mux drive. No such parts.
- **Mode 0 and the 74HC14 in the `CLK` path.** That inverter existed to make the
  rig's '161 advance on the falling edge. Rev D handles phase with the SPI mode
  and needs no inverter — the expected mode is **2**, and it is unverified.
- **The 1–16 MHz clock ceiling and the ~55–60 ns endpoint delay.** Measured on
  HC parts in a counter+mux topology; says nothing about a '165 chain. Re-run
  `PINLED_SPI_SWEEP` on the new rig.

### Outstanding for rev D

**Rev D hardware exists and runs.** A 4× 74HC165 breadboard rig (2 modules,
32 channels) at 4 MHz, driven first by an ESP32-S3-DevKitC-1 (2026-08-06) and
then by a QT Py ESP32-S3 FH4R2 (2026-08-07).

Settled on that rig:

- **SPI mode 2** — `U1.D` → channel 4, `U2.D` → channel 12, exactly. The single
  claim the whole rev D framing rested on.
- **Bit order** — `H` first, channels ascending, no byte reversal.
- **`QH`→`SER` handoff** — within a module *and* across a module boundary.
- **`/PL` from `CS`** — positive-polarity `CS` loads between frames.
- **Frame timing** — 26.8 µs for 32 ch → 37274 Hz free-run, 27% duty at 10 kHz.
- **Self-termination (HW-11)** — last module pulled live; its channels held a
  hard zero across ~4.9 M frames, including while the surviving module was
  actively switching. `num_modules` is a performance setting, not a correctness
  one.

**Every rev D correctness claim is now measured.** What remains is either
diagnostic polish or needs a longer chain than four chips:

1. **`/PL` held low** makes `DATA` track channel 0 live (FR-DIAG-3) — needs a
   `PINLED_SCAN_HOLD_CH=0` build (§5 check 4). Diagnostic convenience only;
   nothing depends on it.
2. **Pins 3, 4, 5 and 13** (`E`, `F`, `G`, `C`) of the '165 have not been
   individually exercised, and are hard-grounded on the rig so it cannot
   exercise them. Channel 0 (pin 6) is done.
3. Everything in `TIMING.md` §7 items 2–4: `/PL` edge quality at 800 mm,
   clock-skew direction, and multi-drop `CLK` integrity. All three need chain
   lengths the bench rig does not have, and all three are about *scaling* a
   design whose correctness is now established at 2 modules.
