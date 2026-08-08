# pinled

Universal, drop-in **LED replacement for incandescent lamps in vintage pinball
machines**. An ESP32 *senses* the machine's original lamp drive at each socket,
reconstructs how bright the original bulb would have been, and reproduces it on
an addressable LED (WS2812B / SK6812). Because the sensing and mapping live in
firmware, one board adapts to Bally, Williams, Stern, Gottlieb, and EM games via
configuration rather than a new PCB per machine.

Project home: https://oneoffendeavors.com/projects/001-pinball-light-strip/

> **v2 firmware.** This replaces the single-mux proof-of-concept with a
> 16-channel-per-module scan front end, a filament-emulating brightness model,
> and an auto-profiler. See [`docs/`](docs/) for the full design.

## How it works

An incandescent bulb is a thermal low-pass filter: brightness is the
time-average of the power delivered, with a ~20-50 ms time constant. Every
pinball lamp-drive scheme (steady EM DC, strobed solid-state matrix, phase-
chopped/dimmed GI) was designed to look right *through that filter*. The
firmware **is** that filter — a fixed-point leaky integrator per channel — so it
does the correct thing for every era automatically.

The analog front end is deliberately fast and dumb: FET level-shifters threshold
each lamp tap to a clean 3.3 V digital pulse train, and the *duty cycle* carries
brightness. The time constant lives in firmware, where it is reprogrammable per
machine.

## Hardware (per 16-channel module)

- **2× 74LVC165** — 8-bit parallel-in / serial-out shift registers, chained
  `QH` → `SER`. That is the entire scan logic: two ICs per module, no counter,
  no muxes, no arbitration, no gating.
- **16× N-ch MOSFET + inverting Schmitt trigger** — per-channel level-shift
  (5-20 V, AC/DC), protection, and hysteresis. Two inversions cancel: lamp on
  reads as logic high.
- **10 kΩ pull-down + 33 Ω series** on `DATA` — the pull-down makes the chain
  self-terminating, the series resistor source-terminates each hop.
- **3.3 V LDO** — every module regulates locally from the 5 V harness rail.
- **ESP32-S3** (Adafruit QT Py ESP32-S3) — **3 GPIO total** for sensing, not per
  module, plus 1 GPIO for the whole LED string.

Modules chain 1..8 on a **5-pin JST-SH harness** (`VCC`, `GND`, `DATA`, `CLK`,
`/PL`) in 100 mm hops, giving **8 to 128 channels on the same three pins**.
Modules are identical and unaddressed. `CLK` and `/PL` are bussed; `DATA` is
point-to-point, so the whole harness behaves as one shift register 16·N bits
deep. See [`docs/CHAINING.md`](docs/CHAINING.md).

A frame is three steps: drop `/PL` (registers transparently follow their
inputs), raise it — **every channel in the chain freezes on that one edge** —
then clock 16·N bits out as a single **SPI + DMA** transaction. `SCLK`=`CLK`,
`MISO`=`DATA`, `CS` wired as `/PL` (positive polarity), mode 2.

One transaction per frame is the point. The ESP-IDF SPI driver costs a measured
~17 µs per transaction regardless of length, so a design that reads eight
addressed devices pays it eight times. See
[`docs/HARDWARE.md`](docs/HARDWARE.md) and
[`docs/TIMING.md`](docs/TIMING.md).

## Firmware layout

```
main/                  app entry + task wiring (ooe::pinled::Main)
components/
  lamp_scan/           chained '165 shift-register scan driver (SPI + DMA)
  filament/            per-channel leaky integrator (filament model)
  profiler/            drive-scheme auto-classifier
  lamp_map/            channel -> LED mapping + WS2812B (RMT) render
  machine_config/      NVS profiles + Kconfig defaults
docs/                  DOSSIER, FIRMWARE_PLAN, REQUIREMENTS, HARDWARE, CHAINING, TIMING, BRINGUP,
                       WEBUI + chain_timing.svg, ui-mockup.html
```

Two FreeRTOS tasks: `scan_task` samples every channel at a fixed 10 kHz and
feeds the integrators; `render_task` pushes LED frames at 60-120 Hz. The
integrator decouples the two rates (and kills matrix-strobe aliasing).

The scan rate is deliberately fixed rather than free-running: the DMA burst is
linear in channel count (12 µs at 16 channels, 68 µs at 128), so pacing is what
makes a filament time constant mean the same thing on a bench rig and a full
playfield. See [`docs/TIMING.md`](docs/TIMING.md).

## Build

Requires ESP-IDF **5.5.x** (a v6.0 preset is also provided).

```sh
idf.py set-target esp32s3
idf.py menuconfig      # pins, channel count, timing under "pinled configuration"
idf.py build flash monitor
```

Pins default to the QT Py ESP32-S3 mapping (`CLK`=18/A0, `/PL`=17/A1,
`DATA`=9/A2, `LED`=8/A3) and are overridable in `menuconfig`.

## Status

First-cut v2. `lamp_scan` and `filament` are implemented; `profiler`,
`lamp_map`, and `machine_config` have working interfaces with documented
algorithms and TODOs for v1.

The design is planned through to the chained-module architecture (8-128
channels) — see [`docs/TIMING.md`](docs/TIMING.md) — and the code is partway
there. The `esp32s3` retarget and pin map are done and verified on hardware
(M0.5), and the sense path is proven end to end on a single breadboard module:
counter, address decode, mux, and polarity all confirmed. See
[`docs/BRINGUP.md`](docs/BRINGUP.md).

Still outstanding: the scan driver bit-bangs one `DATA_IN` per module rather
than clocking a shared bus over SPI, free-runs instead of pacing to a fixed
rate, and the renderer issues one strip transmit per channel instead of one per
frame. Those are milestone M1a in
[`docs/FIRMWARE_PLAN.md`](docs/FIRMWARE_PLAN.md). Module chaining additionally
needs rev B modules — the bench rig uses a push-pull '151, which cannot share a
bus, and has none of the chaining logic.

## License

MIT © 2024-2026 Jefferson J. Hunt. Third-party attributions retained under each
component. LED output uses the `zorxx/neopixel` component.
