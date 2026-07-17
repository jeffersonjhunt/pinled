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

- **74HC161** synchronous counter — `Q0..Q2` select the mux channel, `Q3` bank-selects.
- **2× 74HC251** — 8:1 muxes with **tri-state** outputs, bussed onto one `DATA_IN` line.
- **16× N-ch MOSFET + diode** — per-channel level-shift (5-20 V, AC/DC) + protection.
- **ESP32** (POC: Adafruit QT Py ESP32 Pico) — 3 GPIO/module (`CLK`, `/MR`, `DATA_IN`) + 1 GPIO for the whole LED string.

Why '251 not '151: the '151 is push-pull and can't share a data line; the '251
is tri-state and can. See [`docs/HARDWARE.md`](docs/HARDWARE.md).

## Firmware layout

```
main/                  app entry + task wiring (ooe::pinled::Main)
components/
  lamp_scan/           74HC161 + dual 74HC251 scan driver
  filament/            per-channel leaky integrator (filament model)
  profiler/            drive-scheme auto-classifier
  lamp_map/            channel -> LED mapping + WS2812B (RMT) render
  machine_config/      NVS profiles + Kconfig defaults
docs/                  DOSSIER, FIRMWARE_PLAN, REQUIREMENTS, HARDWARE
```

Two FreeRTOS tasks: `scan_task` samples fast and feeds the integrators;
`render_task` pushes LED frames at 60-120 Hz. The integrator decouples the two
rates (and kills matrix-strobe aliasing).

## Build

Requires ESP-IDF **5.5.x** (a v6.0 preset is also provided).

```sh
idf.py set-target esp32
idf.py menuconfig      # pins, channel count, timing under "pinled configuration"
idf.py build flash monitor
```

Pins default to the QT Py ESP32 Pico mapping (`CLK`=25, `/MR`=27, `DATA_IN`=26,
`LED`=15) and are overridable in `menuconfig`.

## Status

First-cut v2. `lamp_scan` and `filament` are implemented; `profiler`,
`lamp_map`, and `machine_config` have working interfaces with documented
algorithms and TODOs for v1. See [`docs/FIRMWARE_PLAN.md`](docs/FIRMWARE_PLAN.md)
for the bring-up milestones.

## License

MIT © 2024-2026 Jefferson J. Hunt. Third-party attributions retained under each
component. LED output uses the `zorxx/neopixel` component.
