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

- **74LVC161** synchronous counter — `Q0..Q2` select the mux channel, `Q3` bank-selects.
- **2× 74LVC251** — 8:1 muxes with **tri-state** outputs, bussed onto one `DATA` line.
- **16× N-ch MOSFET + inverting Schmitt trigger** — per-channel level-shift
  (5-20 V, AC/DC), protection, and hysteresis. Two inversions cancel: lamp on
  reads as logic high.
- **ESP32-S3** (Adafruit QT Py ESP32-S3) — **3 GPIO total**, not per module,
  plus 1 GPIO for the whole LED string.

Modules chain 1..8 on a 5-pin JST-SH harness (`CLK`, `/MR`, `DATA`, `GND`,
`VIN`) in 100 mm hops, giving **8 to 128 channels on the same four pins**.
Counters cascade via the carry bit and unaddressed modules park their muxes in
high-Z, so every module shares one data line.

Why '251 not '151: the '151 is push-pull and can't share a data line; the '251
is tri-state and can — which is what scales from 2 drivers to 16. See
[`docs/HARDWARE.md`](docs/HARDWARE.md) and
[`docs/TIMING.md`](docs/TIMING.md).

## Firmware layout

```
main/                  app entry + task wiring (ooe::pinled::Main)
components/
  lamp_scan/           74HC161 + dual 74HC251 scan driver
  filament/            per-channel leaky integrator (filament model)
  profiler/            drive-scheme auto-classifier
  lamp_map/            channel -> LED mapping + WS2812B (RMT) render
  machine_config/      NVS profiles + Kconfig defaults
docs/                  DOSSIER, FIRMWARE_PLAN, REQUIREMENTS, HARDWARE, TIMING
```

Two FreeRTOS tasks: `scan_task` samples every channel at a fixed 10 kHz and
feeds the integrators; `render_task` pushes LED frames at 60-120 Hz. The
integrator decouples the two rates (and kills matrix-strobe aliasing).

The scan rate is deliberately fixed rather than free-running: frame time is
linear in channel count (3.2 µs at 16 channels, 23.5 µs at 128), so pacing is
what makes a filament time constant mean the same thing on a bench rig and a
full playfield. See [`docs/TIMING.md`](docs/TIMING.md).

## Build

Requires ESP-IDF **5.5.x** (a v6.0 preset is also provided).

```sh
idf.py set-target esp32s3
idf.py menuconfig      # pins, channel count, timing under "pinled configuration"
idf.py build flash monitor
```

Pins default to the QT Py ESP32-S3 mapping (`CLK`=18/A0, `/MR`=17/A1,
`DATA_IN`=9/A2, `LED`=8/A3) and are overridable in `menuconfig`.

## Status

First-cut v2. `lamp_scan` and `filament` are implemented; `profiler`,
`lamp_map`, and `machine_config` have working interfaces with documented
algorithms and TODOs for v1.

The design is planned through to the chained-module architecture (8-128
channels) — see [`docs/TIMING.md`](docs/TIMING.md) — but the code has not caught
up yet: it still targets `esp32` with the old pin map, reads one `DATA_IN` per
module rather than a shared bus, and free-runs the scan loop. Those are
milestones M0.5-M1c in
[`docs/FIRMWARE_PLAN.md`](docs/FIRMWARE_PLAN.md).

## License

MIT © 2024-2026 Jefferson J. Hunt. Third-party attributions retained under each
component. LED output uses the `zorxx/neopixel` component.
