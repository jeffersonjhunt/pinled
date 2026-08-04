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
| `PINLED_SCAN_STEP_MS` | `0` (off) | Is the counter actually counting? |
| `PINLED_SCAN_HOLD_CH` | `-1` (off) | Where exactly does the signal die? |
| `PINLED_SPI_SWEEP` | `n` (off) | How fast can the chain be clocked? |

`SCAN_HOLD_CH` takes precedence over `SCAN_STEP_MS`. Either one suppresses
`scan_task` — they own the counter position and would otherwise race it — so
**the LED string does not update while either is active.** The log says so at
startup.

> All four work on **production** hardware. Rev C removed the RC activity
> detector in favour of a registered `STARTED` latch, so the chain holds no
> analog state and can be clocked arbitrarily slowly or stopped outright. (Under
> the rev B 4-wire design the two slow modes would have been bench-rig only —
> stopping the clock there released the bus grant.)

## 1. `PINLED_SCAN_DEBUG` — is anything alive?

Logs the raw pre-filament frame twice a second:

```
I (2618) pinled-main: raw [00001111]  tog [----TTTT]
```

- **`raw`** — the bus right now, one character per channel, grouped in eights.
  Group boundaries are mux banks: left is '251 #A (0–7), right is #B (8–15).
- **`tog`** — sticky since boot. `T` means that channel has been observed *both*
  high and low, so it is genuinely switching rather than stuck.

`tog` being sticky is the point: you can press test inputs at your leisure and
read the result later, with no timing to coordinate.

A channel showing `-` forever is unconnected, unpopulated, or pinned by the bus
bias. It is never merely "idle" — an idle lamp still reads a stable 0, and that
low is recorded on the very first frame.

This isolates the sense bus from both the filament model and the LED string, so
a dead strip and a dead bus stop looking identical.

## 2. `PINLED_SCAN_STEP_MS` — is the counter counting?

At full speed the scan runs tens of thousands of frames per second, so the
counter's `Q0` toggles near 800 kHz. **Indicator LEDs on the `Q` outputs cannot
show this** — they sit at a steady half-brightness, which reads as "not
counting" and sends you hunting a fault that isn't there.

Set it to `250` and the scan advances one channel every 250 ms:

```
I (127)  --- /MR pulsed, counter should now read 0000 ---
I (377)  count  0  expect Q3..Q0 = 0000  DATA=0
I (1377) count  4  expect Q3..Q0 = 0100  DATA=1
```

A full 16-count cycle takes 4 s. Expected blink rates:

| Output | Changes every |
|---|---|
| `Q0` | 250 ms |
| `Q1` | 500 ms |
| `Q2` | 1 s |
| `Q3` | 2 s |

If the counter does **not** step, the fault is the '161, and it is almost always
a control pin left floating. All of these must be tied high: `/PE` (pin 9),
`CEP` (pin 7), `CET` (pin 10), and `/MR` (pin 1) except during the reset pulse.
A floating HC input reads either way and will give you intermittent behavior
later even if it happens to work now.

## 3. `PINLED_SCAN_HOLD_CH` — where does the signal die?

The walk sweeps past any given channel for only a few hundred ms per cycle,
which is useless for tracing with a meter. Hold parks the counter on one channel
and stops clocking, so every node becomes a static DC level:

```
I (127) counter parked on channel 4; Q3..Q0 = 0100, '151 C/B/A = 100
I (627) hold ch 4  DATA=1
```

Measure along the chain and find the first link that stops tracking:

| Node | Should read |
|---|---|
| '161 `Q3..Q0` | the channel number in binary |
| mux `C`/`B`/`A` | the low three bits of it |
| mux `D<n>` | your input — follow it with the button |
| mux `Y` | equal to `D<n>`, provided `/E` is low |
| MCU `DATA_IN` | equal to `Y` |

## 4. Interpreting a full-speed frame

With the scan running normally, these signatures come up repeatedly:

| Symptom | Cause |
|---|---|
| All 16 bits move together | `CLK` isn't reaching the '161 — the count never advances, so all 16 samples read one channel |
| Bits 0–7 work, 8–15 never | `QD` bank select: '251 #B never enables |
| Right group mirrors the left | `/E` tied low instead of gated by `QD`; the address wraps and re-reads the same eight inputs |
| Everything reads 1 | `W` (inverted output) wired instead of `Y`, or the bias/internal pull is backwards |
| Everything reads 0 | mux disabled, or `DATA` not connected |

On a **chained** rig, add these:

| Symptom | Cause |
|---|---|
| All channels beyond 16·k read 0 | module k+1 never asserts DONE, so the clock never forwards past it — everything downstream is dead, not just that module |
| Frame tail duplicates the head | chain reset partway through the frame — check `/MR` is not glitching mid-burst |
| Every module's line 0 wrong, rest fine | the arm sample is not being discarded, or `ENP` is not wired to `STARTED` |
| Far modules wrong, near modules fine, worse as modules are added | accumulated clock skew — HC parts, or the SPI clock is too fast (`TIMING.md` §4.3) |
| First channel of module 1 unreliable | `ACT` still charging during the first half-period; clock one dummy bit and discard |
| **Every channel off by one, perfectly stable** | chain clock above the ceiling. Past it the mux output has not propagated when the master samples, so `sample[k]` returns `line[k-1]` — stable and wrong, which reads as a mapping error rather than a timing one. Lower `PINLED_SPI_HZ`. |

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

## 5. Bench substitutions

Until the '251s arrive, a **74x151** works for a single module, with caveats:

- **Omit the 1 kΩ bias resistor.** The '151 is push-pull and always drives; the
  bias only loads it. At 3.3 V into 1 kΩ an HC part's high can sag toward the
  S3's `V_IH` (~2.48 V). Fit the bias when the tri-state parts go in.
- **It cannot chain.** Push-pull outputs can't share a bus — that is the entire
  reason for the '251 (see the comparison table in `HARDWARE.md`). Module
  chaining is untestable until the swap, and the rig also lacks the DONE and
  STARTED latches and the clock gating a real module carries (`CHAINING.md`).
- **Set `PINLED_CHANNELS_PER_MODULE=8`** for a single 8-input mux, otherwise
  channels 8–15 mirror 0–7 and light a second LED per input.
- **Check the supply rail.** GPIO 9 is **not 5 V tolerant**. A '151 on 5 V
  drives 5 V into the S3. A 74LS part needs 5 V and therefore needs level
  shifting; use LVC (HW-3).
- **Add an inverter in the `CLK` path.** Without one the rig clocks its '161
  directly, so the counter advances on the same rising edges SPI samples: every
  sample reads `line[k+1]` and line 0 is unreachable *by any SPI mode*. One
  spare Schmitt section between the MCU `SCLK` pin and the '161 `CLK` makes the
  counter advance on the falling edge, which is what a real module does — and
  then mode 0 aligns exactly. Worth doing: it turns the rig from "validates
  plumbing" into "validates the production sampling phase".

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

Verified on an ESP32-S3-DevKitC-1 (`N8R8`) against a breadboard module built
around a 74x151:

- Counter counts, `/MR` clears, address decode correct at all 16 counts.
- Mux drives `DATA`; test inputs read at the right channel indices.
- Polarity confirmed non-inverting: input high → logic 1, `active_low = n`
  (HW-1).
- No crosstalk onto unconnected channels; stable across 40+ minutes.
- **SPI + DMA scan path** at 1 MHz: peripheral config, GPIO-matrix routing of
  `SCLK`/`MISO`, receive-only transaction, MSB-first unpack, and actual-vs-
  requested clock rate.
- **Mode 0 sampling phase**, with a 74HC14 added in the `CLK` path so the '161
  advances on the falling edge as a real module does. Inputs on D4–D7 read at
  channels 4–7 with no shift, confirming the sample lands mid-window and line 0
  is captured. Before the inverter, modes 0 and 1 both shifted by one — see §5.
- **`/MR` driven from SPI `CS`** with `SPI_DEVICE_POSITIVE_CS` (rev C): `CS` is
  low between transactions (clearing the chain) and high during (counting), so
  the frame reset is hardware-timed and needs no software. Verified by reading
  channels 4–7 correctly, including simultaneous presses.

Outstanding, all dependent on rev C modules: `QD` bank select, the 1 kΩ bias at
the master, module chaining, real lamp taps, and the chain-specific measurements
in `TIMING.md` §7 — the arm clock, `/MR` reach, and accumulated clock skew.

Note the bench rig predates the chaining design: it has no DONE or STARTED
latch and no clock gating, so it exercises the counter, mux, polarity and the
SPI path but nothing of the chaining protocol itself.
