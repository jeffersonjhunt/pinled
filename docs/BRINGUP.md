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

The sweep enforces both itself now rather than leaving them to the reader. The
slowest working rate becomes the **reference** and every faster rate is judged
against it, and a rate whose union is empty is reported as `ALL ZEROS —
proves nothing` instead of a perfect score. The first version did neither, so
a rig with nothing held printed `stable 256/256` from 1 to 40 MHz and read as
a clean bus at 40 MHz.

### SN74LVC165 result, 2026-08-14

The rev D rig rebuilt with SN74LVC165 in place of 74HC165, 4 chips / 2 modules
/ 32 channels at 3.3 V, `U4.D` tied high as a steady far-end test bit:

**Every rate matched the reference, 1 MHz through 40 MHz** — 8 passes, 96 rate
measurements, 24 576 frames, one identical pattern throughout, no unstable
rates and no mismatches. 40 MHz is the top of the ladder because it is the
**ESP32-S3's** limit through the GPIO matrix (`SCLK`=18, `MISO`=9, neither an
IOMUX SPI pin), so this found the MCU's ceiling and not the part's. The HC
parts were qualified at 4 MHz.

**What it does not show, and the reason to be careful with it.** 28 of the 32
inputs are hard-grounded and one is tied high, so `DATA` carries 31 zeros and a
single 1 — **two transitions in a 32-bit frame**, the sparsest pattern the bus
will ever carry. That is a real result for what it covers: clock generation,
the CPHA=0 capture instant, `/PL`, traversal of all four chips, and no frame
slip, since a dropped or doubled bit would move that 1. It is **not** a
signal-integrity qualification. The stress case is maximum transition density —
`10101010`, where `DATA` toggles at half the clock rate through the 33 Ω series
resistor on a breadboard — and nothing here exercises it.

To close that gap, tie `H`, `F`, `D`, `B` high on **U4** (lifting `F` and `B`
off ground), giving `10101010` on channels 24–31 at the far end of the chain,
and re-run. That is four wires on one chip, and it does something else worth
having: `F` (pin 4) and `B` (pin 12) are on the list of inputs this rig has
never exercised, so the same rework closes part of §7's outstanding item 1.

**Do not raise HW-3's 4 MHz default on this evidence.** It is a 4-chip
breadboard, and `TIMING.md` §4.3 says multi-drop `CLK` integrity across 800 mm
is the binding constraint — a different measurement on hardware that does not
exist yet. What this establishes is that the **chips** have stopped being the
limit.

## 5. Building the rev D bench rig

Two '165s prove one module; **four prove the design**, because the open question
is the chain handoff and the terminator, not the register. Build two modules.

### Shopping list

| Qty | Part | Notes |
|---:|---|---|
| 6 | **74HC165N**, DIP-16 | 4 needed, 2 spare. HC not LVC: LVC is not made in through-hole. Run at **3.3 V**, up to **4 MHz** (HW-3, relaxed from 2 MHz after this rig ran clean at 4 MHz for millions of frames). |
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
4. ~~**`/PL` held low.**~~ **PASSED 2026-08-07.** A `PINLED_SCAN_HOLD_CH=0`
   build logged `DATA=0` at idle, `DATA=1` for the ~24 s the channel-0 button
   was held, and `DATA=0` on release — one clean transition each way with no
   chatter, and no clocking involved at all. The registers really do stay
   transparent and follow the input. FR-DIAG-3 works as designed.
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

## 5b. Verifying the profiler re-arm

Everything about the re-arm except the one claim that matters can be checked
from a wired build host. The claim that cannot is **that classification tracks
what the machine is doing** — which needs a channel physically held while a
pass runs, and therefore needs a person at the rig.

It takes about a minute. Watch the classes in one terminal:

```sh
tools/pbtool.py live ws://pinled.local/api/v1/live --classes --seconds 120
```

That prints one letter per channel — `.` off, `S` steady, `M` matrix,
`A` ac_steady, `D` ac_dimmed — and only when the row **changes**, so a
completed pass is one new line in an otherwise still terminal.

| # | Do | Expect |
|---|---|---|
| 1 | Nothing. Read the first row. | Mostly `.`: the boot pass saw a dark playfield. |
| 2 | **Hold** a wired input down (`U1.D` is channel 4). | Row unchanged. The class is stale by design — this is the bug being fixed. |
| 3 | Still holding, `curl -X POST http://pinled.local/api/v1/profiler` | ~0.8 s later, one new row: that channel becomes `S`. |
| 4 | Release. POST again. | ~0.8 s later, back to `.`. |
| 5 | Click the BOOT button — a normal press, anything under 5 s — while holding an input. | Same as 3, from the button. The log says `auto-profiling: window 750 ms` with no `re-armed by API request` beside it, which is how you tell the two apart. A press under 150 ms is ignored and logs `press … was too short`. |
| 6 | Repeat 3 on **`U3.D`, channel 20**, while holding it. | Stays `A`. That channel is `class_lock: AC_STEADY` in `fs_seed/profile.pb.json`, and the profiler is not allowed to touch it (FR-PROF-4, FR-CFG-8). |
| 7 | POST twice in quick succession. | Second returns **409** with the in-flight status as its body. |
| 8 | Hold BOOT the full 5 s. | Still erases the network and reboots into SoftAP — the split by duration did not break the rescue. |

Step 4 is the half that makes this a test rather than a demonstration: a class
that only ever moves one way would pass steps 1–3 while being a latch rather
than a classifier.

Step 6 is the other half. Steps 3 and 4 only show that the classifier *can*
change a channel; a re-arm that ignored the lock table would pass both while
quietly overwriting every hand-set lamp on the playfield. Channel 20 standing
at `A` through a pass that is actively reclassifying its neighbours is the
evidence that it does not — and it is free, because the bench profile already
locks it.

Note which channel is which before reading a row: on this rig the wired inputs
are the `D` button (pin 14) of each '165, which are channels **4, 12, 20 and
28** — plus `U1.H`, channel 0. `U3.D` is channel 20 because module 1 starts at
channel 16 and `D` is the fifth input in `H G F E D C B A` order.

If step 3 does nothing, check `GET /api/v1/profiler` first — `state` and
`passes` tell you whether the pass ran at all, which separates "the profiler
did not re-arm" from "it re-armed and decided the same thing".

## 5c. Swapping in a board with a different flash size

Done once, 2026-08-15, from the 4 MB FH4R2 to the 8 MB FN8C0 for M4. The table
change is the easy half; the traps are around it.

**A factory-fresh Adafruit board cannot be flashed without touching it.** It
runs Adafruit's firmware, whose USB CDC refuses the DTR/RTS toggling esptool
uses to enter download mode — every attempt, including a plain port open, fails
with `OSError: [Errno 71] Protocol error`. Nothing is wrong; there is simply no
software path in. Hold **BOOT**, press and release **RESET**, release BOOT.

**Then it will not come back out.** Entering download mode by hand latches a
force-download flag that survives the `Hard resetting via RTS pin` at the end of
a flash, so the board reboots straight back into `boot:0x0 (DOWNLOAD)` and looks
like firmware that never started. **Unplug and replug the USB cable** — that is
the only thing that clears it. §6 has the signature.

Order that worked, once it was in download mode:

```sh
python -m esptool --port /dev/ttyACM0 --before no_reset --after no_reset flash_id   # confirm the part
python -m esptool --port /dev/ttyACM0 --before no_reset --after no_reset erase_flash
# CONFIG_PINLED_SEED_FS=y for this one flash only
idf.py -p /dev/ttyACM0 flash        # bootloader + table + app + storage image
# replug, verify, then CONFIG_PINLED_SEED_FS back off and flash again
```

`flash_id` is the check that matters and the only one that keeps working: it
reports `Embedded Flash 8MB` and whether PSRAM appears in `Features`. USB
VID/PID does **not** identify the variant once our firmware is on — everything
reports `303a:1001` from then on.

**Budget for being unprovisioned.** The new table erases NVS and the
filesystem, and the build-time Wi-Fi credentials were deleted in M3 step 7, so
the board comes up in SoftAP and the only way back is the captive portal. A new
board also means a new MAC, hence a new `device_id`, a new
`pinled-XXXXXX` and a new address. The AP name uses the *AP interface* MAC,
which is the base MAC **plus one** — a board whose chip MAC ends `7E:54`
advertises `pinled-6E7E55`, and that is correct rather than a typo.

That erase is worth treating as an opportunity: it is the only cheap chance to
exercise the portal on a device that has genuinely never been provisioned,
which is the one state it exists for. It worked first time here — scan at boot
found and de-duplicated three networks, the page popped by itself, and
`credentials: provisioned for "…"` names the SSID and never the password.

**Turn `PINLED_SEED_FS` back off afterwards, and confirm it.** With it on every
`idf.py flash` overwrites the stored configuration. The check is that
`build/flash_args` stops mentioning `storage.bin` and that the next boot still
logs both documents loading — which also verifies the useful half: an app flash
leaves the filesystem alone.

### What the swap verified, 2026-08-15

Everything M3 and M3+ claim, re-run on the new silicon: **11/11 on
`tools/rearm_check.py --include-rescue`**, which is the §5b checklist plus the
destructive step it normally skips.

The one that had never been re-tested is `FR-UI-7`, the long-hold rescue —
`rescue.cpp` had changed twice since it last ran (the short-press callback,
then the 20 Hz poll and 150 ms threshold). The full trail is in the log: the
countdown at 4, 3, 2, 1 seconds, `erasing credentials and restarting into
SoftAP`, the AP coming up, a phone joining, `provisioned for "…"`, and the
rejoin. Splitting one button between two jobs by duration did not break either
job.

Also confirmed here rather than assumed: the profiler and the live monitor
behave identically on the FN8C0 as on the FH4R2 — same 750 ms window, same
7500 frames, 30.2 Hz push with no sequence gaps, the locked channel still
locked. The MCU swap changed the flash size and the MAC and nothing else.

## 5d. Verifying the status pixel

The indicator is the one part of this firmware whose correctness cannot be
read off a log: the log says what the pattern generator was *asked* for, and
the question is what a person standing at the board actually sees. The
pattern generation itself is covered by 31 host cases, so what is left here is
genuinely only the eye and the hand — is it lit, is it the right colour, does
the button feel acknowledged.

On the QT Py bench board the pixel is the onboard NeoPixel: **GPIO 39, with
power-enable on GPIO 38**. It is dark without 38 driven high, and that looks
exactly like a dead driver — check the boot line before suspecting anything
else:

```
I (237) indicator: status pixel on GPIO 39 (power enabled) (RMT), brightness 64/255, byte order GRB (default)
```

RMT, not I2S. The playfield string uses zorxx/neopixel, which is an I2S
driver, and the S3 has two I2S controllers against four RMT channels — the
first version of this shared the I2S driver and the board complained on the
first boot (`i2s controller 0 has been occupied`). If that warning ever comes
back, something has moved the status pixel back onto the strip's driver.

| # | Do | Expect |
|---|---|---|
| 1 | Power on and watch from reset. | **White** while booting, then **dim green** once the API line prints. The whole boot is about 3 s, so the white is brief. |
| 2 | Nothing, for a minute. | **Perfectly still.** FR-IND-3: a healthy machine does not move. Anything blinking here is a fault, and the log names it. |
| 3 | Pull the network (or boot with the router down). | **Green, breathing** at about 0.5 Hz — works, not reachable. It never goes fully dark, which is what separates it from a blink. |
| 4 | Hold BOOT the full 5 s, let it reboot unprovisioned. | **Blue, fast blink.** Blue rather than green because it is a mode awaiting action, and it keeps the two states you must tell apart off the red/green axis entirely (FR-IND-2). |
| 5 | Click BOOT — a normal press. | An **80 ms white blip the instant the press registers**, then the profiler pass's own brief white flash. |
| 6 | Tap BOOT faster than 150 ms. | **Nothing.** The absence is the answer (FR-IND-7); the log gives the duration. |
| 7 | Hold BOOT and watch, releasing at ~4 s. | From 1 s: **red, blinking faster each second.** Release and it returns to whatever it was showing, with no trace of the ramp. |
| 8 | `PINLED_STATUS_BRIGHTNESS=0`, rebuild, boot — or set `indicator.brightness: 0` in the install config, no rebuild needed (FR-IND-5). | **Dark, in every state including a fault.** A backbox the owner asked to be dark stays dark; the API still reports the fault. The Kconfig value is only the build default; a stored install overrides it a moment after the store loads, so with both set the stored one wins. |

Step 5 and step 6 are the pair that matters, and they have to be run together.
Either alone is meaningless: a blip that fires on every press including the
ones too short to act on would answer nothing, and silence on every press
would too. What makes the acknowledgement useful is that it fires **exactly
when the press was accepted** — which is why it is wired to the debounce
threshold in `rescue.cpp` and not to `on_short_press`, where it would only
fire on release once the outcome was already known.

Step 7's red deliberately overlaps the fault colour. It is a destructive
action one second away and red is what that deserves; it is distinguishable
because it only happens while the button is held, and because it accelerates.

### Reading a fault

*N* red blinks, a 1.2 s pause, repeat. Counting starts at **2** — one blink
and a pause reads as a heartbeat rather than a count — and where several are
active the **lowest** is shown, with `GET /api/v1/info` reporting all of them.

| Blinks | Class | Where to look first |
|---|---|---|
| 2 | Sense bus | `CLK INH` floating, or a missing `QH`→`SER`. The top two rev D wiring faults. |
| 3 | Configuration | A stored document was rejected — CRC, decode, or a value out of range. Running on defaults; re-apply from the UI. |
| 4 | LED string | Strip init failed or the geometry is impossible. Data line and LED count. |
| 5 | Storage | The filesystem would not mount or would not write. Run `PINLED_STORE_SELFTEST`. |
| 6 | Internal | Out of memory, or a task or the HTTP server would not start. A firmware bug, not a wiring fault. |

Two of these are easy to stage and worth staging once, because a fault path
nobody has ever seen is a fault path nobody has ever seen:

- **3, configuration**: corrupt a stored document — `printf 'x' | dd of=... conv=notrunc` on `/cfg/install.pb` through the API, or seed a deliberately bad fixture. The device should come up on defaults, blinking 3, with the API still answering.
- **5, storage**: not currently reachable without physically breaking the filesystem, which is why it is listed last.

**Classes 2, 4 and 6 are not reachable today**, and that is a real gap rather
than an oversight in this table: every path that would raise them sits behind
an `ESP_ERROR_CHECK` in `Main::init()`, which panics before the pixel can show
anything. A board that cannot bring up its scan hardware currently boot-loops
rather than sitting there blinking twice, which is the opposite of what
FR-IND-4 asks for — "red means *not doing what you asked*", not "dead".
Making those failures survivable is its own change, because each one needs the
subsystems downstream of it to tolerate an uninitialised peer.

## 6. Tooling traps

These are not hardware faults, but they present as one.

**The IDF is not at `~/esp/esp-idf` on the build host.** It is installed by the
Espressif IDE with per-version roots, and `export.sh` from the version
directory **fails** — it looks for a Python venv at a path the installer does
not use. The activation scripts are the supported entry point:

```sh
. ~/.espressif/tools/activate_idf_v5.5.sh     # v6.0 is also installed
```

Sourcing the wrong one, or `export.sh`, leaves `idf.py` undefined and the error
names a missing venv rather than the actual mistake.

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
  and needs no inverter — the mode is **2**, measured on the rev D rig
  2026-08-06/07 and recorded below.
- **The 1–16 MHz clock ceiling and the ~55–60 ns endpoint delay.** Measured on
  HC parts in a counter+mux topology; says nothing about a '165 chain. The
  sweep has since been re-run on the rev D rig: 4 MHz qualified on 74HC165,
  and the SN74LVC165 rebuild matched its reference at every rate from 1 to
  40 MHz (§4b). Neither result raises HW-3's 4 MHz default, for the reasons
  §4b gives.

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

Added 2026-08-14, when the rig was rebuilt with **SN74LVC165** parts and a real
front end:

- **Clock ceiling** — every rate from 1 to 40 MHz matched its reference, with
  the caveats in §4b. The chips have stopped being the limit; 40 MHz is the
  ESP32-S3's own ceiling through the GPIO matrix.
- **HW-1, the front end** — and this is the first time it has existed on a
  bench at all. The earlier rig wired buttons straight to 3V3 with pull-downs
  and had no front end, so the shipping topology was design-only. Now: a 2N7000
  common-source level shift (inverting) into a 74LVC14 inverting Schmitt,
  non-inverting overall, on `U1.H`, `U1.D`, `U2.D`, `U3.D` and `U4.D`.

  Measured with hands clear: idle reads all zeros, and each of the five
  presses lands on channels 0, 4, 12, 20 and 28 — **one channel per press, no
  neighbours**. `active_low` stays off, the 27 tie-offs stay at GND and the
  DATA bias resistor stays a pull-down, which is the three-way consistency the
  `PINLED_ACTIVE_LOW` help describes.

  The FET alone is inverting, and wiring it without the Schmitt is a
  believable shortcut: the symptom is those five channels reading permanently
  *on* while the other 27 read correctly. Do not reach for `active_low` to fix
  it — it is one global XOR, so it would correct five channels and invert
  twenty-seven, and it also flips the sense of HW-11's self-termination.

  **Still not exercised: the level shift itself.** The Schmitt and the
  inversion are proven; driving the FET gate from a real 5–20 V lamp rail
  through the divider is a separate test and the trip point has never been
  measured.

- **Long-run frame loss, first measurement, 2026-08-15.** 4.2 hours of uptime
  produced **6000 scan overruns — 0.004%, about 1 frame in 25 000** — at a
  steady rate (1000 per 2100–2470 s, not accelerating, so not a leak). One
  missed 100 µs frame against a 30 ms attack constant is 0.3% of a time
  constant, which is nothing in brightness terms. No watchdog trips, no heap
  warnings, no reboots.

  Worth having because every previous capture was seconds to minutes, so the
  counter had never had a chance to say anything. Not attributed: `SCAN_DEBUG`
  was on, and its three array writes per channel per frame inside the scan
  loop are a plausible contributor, as are Wi-Fi interrupts. Separating them
  needs an A/B nobody has run.

> **Sticky flags set during live wiring are not evidence.** The first capture
> after fitting the '14 showed `T` on channels 3 and 27 as well as the five
> real ones. Both are input `E` (pin 3), both on chips being handled, and both
> are hard-grounded and so cannot legitimately toggle. The tell is that
> **neither ever appeared in a logged `raw` frame** — `tog` is set from every
> frame at 10 kHz while `raw` prints twice a second, so a brush lasting a few
> frames sets the flag permanently and is essentially never caught in a
> snapshot. Reset with hands clear and both were gone. When the two disagree,
> believe `raw`.

**Every rev D correctness claim is measured, and all four diagnostics
(FR-DIAG-1..4) are exercised on real hardware.** What remains needs either a
longer chain than four chips or a rig rewire:

1. **Pins 3, 4, 5 and 13** (`E`, `F`, `G`, `C`) of the '165 have not been
   individually exercised, and are hard-grounded on the rig so it cannot
   exercise them. Channel 0 (pin 6) is done.
2. Everything in `TIMING.md` §7 items 2–4: `/PL` edge quality at 800 mm,
   clock-skew direction, and multi-drop `CLK` integrity. All three need chain
   lengths the bench rig does not have, and all three are about *scaling* a
   design whose correctness is now established at 2 modules.
