# Firmware Requirements — `pinled` v2

Requirement IDs are stable; status is `MVP` (first working build), `v1`
(feature-complete for release), or `future`. "SHALL" = required, "SHOULD" =
desired, "MAY" = optional.

## 1. Sensing / scan

| ID | Req | Status |
|---|---|---|
| FR-SCAN-1 | The system SHALL scan lamp channels via a 74HC161 counter driving one or more 74HC251 muxes, reading channel state on ESP32 GPIO. | MVP |
| FR-SCAN-2 | The system SHALL support 16 channels per module (dual '251, Q3 bank-select on a shared tri-state `DATA_IN`). | MVP |
| FR-SCAN-3 | The system SHALL support N modules on a shared `CLK`/`/MR` bus, one dedicated `DATA_IN` GPIO per module. | v1 |
| FR-SCAN-4 | A scan frame SHALL assert `/MR`, then read-then-clock counts 0..(channels-1). | MVP |
| FR-SCAN-5 | Per-channel raw sample rate SHALL be configurable and default to ≥ 2 kHz. | MVP |
| FR-SCAN-6 | Channel active polarity SHALL be configurable (`ACTIVE_LOW` default, for inverting FET front end). | MVP |
| FR-SCAN-7 | The scan loop SHOULD be movable off bit-bang to a timer/`dedic_gpio`/RMT-driven clock without changing the integrator interface. | future |

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

## 4. LED output / mapping

| ID | Req | Status |
|---|---|---|
| FR-LED-1 | The system SHALL drive a WS2812B/SK6812 string from a single GPIO (RMT). | MVP |
| FR-LED-2 | A mapping table SHALL relate sensed channel → LED index(es) + base color. | MVP |
| FR-LED-3 | LED frame/refresh rate SHALL be configurable (default 60–120 Hz) and decoupled from scan rate. | MVP |
| FR-LED-4 | Per-lamp color/tint (e.g. warm-white for GI, colored inserts) SHALL be configurable. | v1 |
| FR-LED-5 | The renderer SHOULD gamma-correct and dither low levels to avoid visible stepping. | v1 |

## 5. Configuration / profiles

| ID | Req | Status |
|---|---|---|
| FR-CFG-1 | Machine profiles (channel map, colors, integrator params, profiler locks) SHALL persist in NVS. | v1 |
| FR-CFG-2 | Build-time defaults SHALL be settable via Kconfig (`idf.py menuconfig`). | MVP |
| FR-CFG-3 | Profiles SHOULD be loadable/exportable as a human-readable text/JSON blob. | future |
| FR-CFG-4 | The system SHALL boot to sane defaults with no stored profile. | MVP |

## 6. Non-functional

| ID | Req | Status |
|---|---|---|
| NFR-1 | Target ESP-IDF **5.5.x**, IDF_TARGET `esp32` (S3 compatible). | MVP |
| NFR-2 | Reusable logic (scan, filament, profiler) SHALL be ESP-IDF components with public headers under `include/`. | MVP |
| NFR-3 | Code style SHALL follow the existing repos: C++, `ooe::pinled` namespace, Doxygen headers, `esp_err_t` returns, `ESP_LOG*` with per-file `TAG`, `ESP_ERROR_CHECK`. | MVP |
| NFR-4 | No dynamic allocation in per-sample / per-frame hot paths. | v1 |
| NFR-5 | BOM target ≤ ~$25/strip; original ESP32-class MCU. | v1 |
| NFR-6 | License MIT; third-party attributions retained under `licenses/`. | MVP |

## 7. Out of scope (first cut)
- OTA / Wi-Fi provisioning UI, web config portal.
- Reverse-engineering per-title lamp tables (baseline behavior comes from
  sensing, not decoding ROM state).
- PCB/gerber design (tracked in the hardware repo, not firmware).
