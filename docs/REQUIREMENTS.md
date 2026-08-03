# Firmware Requirements — `pinled` v2

Requirement IDs are stable; status is `MVP` (first working build), `v1`
(feature-complete for release), or `future`. "SHALL" = required, "SHOULD" =
desired, "MAY" = optional.

## 1. Sensing / scan

| ID | Req | Status |
|---|---|---|
| FR-SCAN-1 | The system SHALL scan lamp channels via a 74x161 counter driving one or more 74x251 muxes, reading channel state on ESP32 GPIO. | MVP |
| FR-SCAN-2 | The system SHALL support 16 channels per module (dual '251, Q3 bank-select on a shared tri-state `DATA_IN`). | MVP |
| FR-SCAN-3 | The system SHALL support 1..8 modules chained on one bus — counters cascaded via TC, module index decoded from the counter's upper bits, unaddressed '251s in high-Z — sharing a single `CLK`, `/MR`, and `DATA_IN`. | MVP |
| FR-SCAN-4 | A scan frame SHALL assert `/MR`, then read-then-clock counts 0..(channels-1) across the whole chain. Any module count 1..8 SHALL be legal. | MVP |
| FR-SCAN-5 | Per-channel raw sample rate SHALL be configurable and default to 10 kHz. | MVP |
| FR-SCAN-6 | Channel active polarity SHALL be configurable, defaulting to **active-high** (FET + inverting Schmitt trigger = non-inverting front end). | MVP |
| FR-SCAN-7 | The scan loop SHALL use the S3 `dedic_gpio` peripheral for `CLK`/`/MR`/`DATA_IN`, with the bundle created on the scan task's own core. | MVP |
| FR-SCAN-8 | Sampling SHALL be paced to a fixed configured rate independent of channel count, not free-run. | MVP |
| FR-SCAN-9 | The driver SHALL measure actual frame time at boot, clamp the configured rate to what the hardware sustains at ≤ 60% core occupancy, and publish the resulting rate as the authoritative Fs for all downstream time-constant math. | MVP |
| FR-SCAN-10 | Settle timing SHALL be two-tier: a short in-module settle and a longer settle at module boundaries and frame start. | MVP |

## 2. Filament (brightness reconstruction)

| ID | Req | Status |
|---|---|---|
| FR-FIL-1 | Each channel SHALL run a leaky integrator emulating an incandescent filament's thermal response. | MVP |
| FR-FIL-2 | Attack (rise) and decay (fall) time constants SHALL be independently configurable, default ~20–50 ms. | MVP |
| FR-FIL-3 | The integrator SHALL accept normalized duty (0..1) or per-sample boolean and output brightness 0..255. | MVP |
| FR-FIL-4 | A configurable output gamma/curve SHALL map integrator level → LED PWM to match perceived bulb response. | v1 |
| FR-FIL-5 | Math SHALL be fixed-point (no float in the hot loop) and run for ≥ 64 channels within frame budget. | MVP |

## 3. Auto-profiling

| ID | Req | Status |
|---|---|---|
| FR-PROF-1 | The system SHALL classify each channel as STEADY / MATRIX / AC_STEADY / AC_DIMMED / OFF from observed transitions. | v1 |
| FR-PROF-2 | Profiling SHALL run at boot and be re-armable at runtime. | v1 |
| FR-PROF-3 | Classifier output `{class, duty_norm, period_est, confidence}` SHALL seed each channel's integrator gain/attack/decay. | v1 |
| FR-PROF-4 | A machine profile MAY override or lock per-channel class/params. | v1 |
| FR-PROF-5 | The observation window SHALL be specified in milliseconds (default 500 ms–1 s) and the frame count derived from the measured Fs, so the window spans several AC and matrix periods at any channel count. | v1 |
| FR-PROF-6 | The classifier SHOULD be robust to correlated false edges from solenoid-induced ground bounce (e.g. discard frames in which an implausible fraction of channels transition together). | v1 |

## 4. LED output / mapping

| ID | Req | Status |
|---|---|---|
| FR-LED-1 | The system SHALL drive a WS2812B/SK6812 string from a single GPIO (RMT). | MVP |
| FR-LED-2 | A mapping table SHALL relate sensed channel → LED index(es) + base color. | MVP |
| FR-LED-3 | LED frame/refresh rate SHALL be configurable (default 60–120 Hz) and decoupled from scan rate. | MVP |
| FR-LED-4 | Per-lamp color/tint (e.g. warm-white for GI, colored inserts) SHALL be configurable. | v1 |
| FR-LED-5 | The renderer SHOULD gamma-correct and dither low levels to avoid visible stepping. | v1 |
| FR-LED-6 | The renderer SHALL build one frame and issue a single strip transmit per refresh, not one transmit per channel. | MVP |
| FR-LED-7 | A global brightness/current cap SHALL bound worst-case LED draw (~6 A at 128 LEDs full-on). | v1 |
| FR-LED-8 | `led_count` × per-LED transmit time SHALL be validated against `refresh_hz` at boot and the refresh rate clamped if it does not fit. | MVP |

## 5. Configuration / profiles

| ID | Req | Status |
|---|---|---|
| FR-CFG-1 | Machine profiles (channel map, colors, integrator params, profiler locks) SHALL persist in NVS. | v1 |
| FR-CFG-2 | Build-time defaults SHALL be settable via Kconfig (`idf.py menuconfig`). | MVP |
| FR-CFG-3 | Profiles SHOULD be loadable/exportable as a human-readable text/JSON blob. | future |
| FR-CFG-4 | The system SHALL boot to sane defaults with no stored profile. | MVP |

## 6. Diagnostics

Bring-up aids. All default to off/normal so production behavior is unchanged;
see `BRINGUP.md` for how they are used together.

| ID | Req | Status |
|---|---|---|
| FR-DIAG-1 | The firmware SHALL be able to log the raw pre-filament channel bitmap plus a sticky per-channel "has been seen both high and low" flag (`PINLED_SCAN_DEBUG`), isolating the sense bus from the filament model and the LED string. | MVP |
| FR-DIAG-2 | The firmware SHALL be able to advance the scan at a human-visible rate (`PINLED_SCAN_STEP_MS`), logging each count and the counter state to expect, because at full speed the counter outputs toggle far too fast to observe. | MVP |
| FR-DIAG-3 | The firmware SHALL be able to park the counter on a single channel indefinitely (`PINLED_SCAN_HOLD_CH`) so every node from the counter outputs to the MCU pin is a static level a meter can read. | MVP |
| FR-DIAG-4 | Modes that take ownership of the counter SHALL suppress `scan_task` rather than race it, and SHALL say so in the log. | MVP |

## 7. Non-functional

| ID | Req | Status |
|---|---|---|
| NFR-1 | Target ESP-IDF **5.5.x**, IDF_TARGET `esp32s3`. Reference board is the Adafruit QT Py ESP32-S3; the ESP32-S3-DevKitC-1 is a supported bring-up board (same GPIO numbers, see `HARDWARE.md`). Neither is the shipping form factor — the product places a bare S3 on the mainboard. | MVP |
| NFR-2 | Reusable logic (scan, filament, profiler) SHALL be ESP-IDF components with public headers under `include/`. | MVP |
| NFR-3 | Code style SHALL follow the existing repos: C++, `ooe::pinled` namespace, Doxygen headers, `esp_err_t` returns, `ESP_LOG*` with per-file `TAG`, `ESP_ERROR_CHECK`. | MVP |
| NFR-4 | No dynamic allocation in per-sample / per-frame hot paths. | v1 |
| NFR-5 | BOM target ≤ ~$25/strip; original ESP32-class MCU. | v1 |
| NFR-6 | License MIT; third-party attributions retained under `licenses/`. | MVP |
| NFR-7 | The CPU SHALL run at 240 MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240`). Every figure in `TIMING.md` §2 derives from it and the IDF default for `esp32s3` is 160 MHz, which inflates the frame budget by 1.5×. | MVP |

## 8. Hardware contract

Properties of the board the firmware depends on. Changing one of these is a
firmware change, not just a hardware change — see `TIMING.md` for derivations.

| ID | Req | Status |
|---|---|---|
| HW-1 | The front end SHALL be non-inverting overall (level-shift MOSFET followed by an *inverting* Schmitt trigger, e.g. 74LVC14): lamp on → logic high. This sets the `active_low` default. | MVP |
| HW-2 | The shared `DATA_IN` SHALL carry a ~1 kΩ bias resistor oriented so that a floating or unpopulated bus reads *lamp off* — a pull-**down**, given HW-1. The MCU's internal pull on that pin SHALL be configured to match rather than left at the `gpio_reset_pin()` default (pull-up). HW-1, HW-2 and the internal pull are one decision. | MVP |
| HW-3 | Muxes and counters SHALL be LVC/LV family at 3.3 V. HC at 3.3 V cannot hold a valid output level against the 1 kΩ bias. | MVP |
| HW-4 | Modules SHALL chain on a 5-pin JST-SH harness: `CLK`, `/MR`, `DATA`, `GND`, `VIN`, in ~100 mm hops, up to 8 modules / 800 mm. | MVP |
| HW-5 | Each module SHALL carry local decoupling (100 nF per IC + ~10 µF bulk); `CLK` SHALL have ~100 Ω series termination at the source. | MVP |
| HW-6 | pinled ground SHALL tie to the machine's lamp-return ground at a single point near the lamp matrix return. | MVP |
| HW-7 | The input SHALL survive rail sag from solenoid firing (series Schottky + bulk capacitance + kickback clamping) without an MCU brownout reset. | MVP |
| HW-8 | Distribution SHOULD be 5 V with local 3.3 V LDOs; higher rails require a single switching regulator at the controller, not per-module switchers adjacent to the sense bus. | v1 |
| HW-9 | The LED data line SHOULD be level-shifted to meet WS2812B V_IH (0.7 × VDD), or the strip run at a reduced VDD. | v1 |

## 9. Out of scope (first cut)
- OTA / Wi-Fi provisioning UI, web config portal.
- Reverse-engineering per-title lamp tables (baseline behavior comes from
  sensing, not decoding ROM state).
- PCB/gerber design (tracked in the hardware repo, not firmware).
