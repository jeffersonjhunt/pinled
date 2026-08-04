> ## Status: evaluated, not adopted — 2026-08-04
>
> This document is retained as the record of an option that was considered and
> rejected. **The adopted design is rev D**, a chain of 74x165 shift registers —
> see [`CHAINING.md`](CHAINING.md).
>
> The proposal is right that rev C's scan logic wanted collapsing, and its wire-
> time arithmetic below is sound. What decided against it:
>
> 1. **Eight SPI transactions per frame.** The MCP23S17 requires `CS` to toggle
>    between devices, so 128 channels cannot be one transaction. Measured on this
>    project, the ESP-IDF driver costs **~17 µs of fixed overhead per
>    transaction** regardless of length (`TIMING.md` §2.4) — so a frame is
>    8 × (17 + 3.2) ≈ **162 µs**, a 6.2 kHz free-run that the boot check clamps
>    to ~3.7 kHz against a 10 kHz requirement. The estimate of "~2–5 µs per
>    transaction" in §Timing Analysis is roughly 4× optimistic. A '165 chain does
>    128 channels in **one** transaction, 49 µs.
> 2. **The harness grows.** The MCP needs `MOSI` as well, so the inter-module
>    connector goes from 5 conductors to 6 or 7 — and busses four fast signals
>    with eight stubs where rev D busses one. "Wavelength ~30 m, no issue" is the
>    wrong test at 10 MHz; edge rate is what matters.
> 3. **`HAEN=0` is the power-on default**, and a device in that state ignores its
>    address pins and drives `SO` on every read. One browned-out or hot-plugged
>    module therefore corrupts **all 128 channels**, not just its own 16.
> 4. **Sample skew.** Reading serially, channel 0 and channel 127 land ~160 µs
>    apart. Rev C's spread was 35 µs; rev D captures everything on one edge.
>
> Two corrections to the text below, independent of the verdict: the
> interrupt-driven alternative is wrong for this application (pinball lamp
> matrices strobe continuously, so the inputs are never static, and it destroys
> the uniform `dt` the filament integrator assumes), and the pin map is for an
> ESP32-WROOM-32 — on the S3, GPIO 19/20 are USB and 26–37 are flash/PSRAM.
>
> **One thing could still revive it:** queueing all eight reads with
> `spi_device_queue_trans()` so the driver's ISR chains them back-to-back may
> amortise most of the 17 µs. That has not been measured. If it works, one IC per
> 16 channels deserves a second look.

# ESP32 + 8× MCP23S17: 128-Channel Digital Input via SPI

## Overview

A single ESP32 reads 128 digital inputs at ≥10 kHz sample rate using 8 daisy-chained
MCP23S17 16-bit SPI I/O expanders sharing one SPI bus and one CS line.

---

## System Specifications

| Parameter              | Value                        |
|------------------------|------------------------------|
| Total input channels   | 128 (8 × 16)                |
| Sample rate target     | ≥10 kHz (all 128 channels)  |
| Interface              | SPI (Mode 0,0)              |
| SPI clock              | 10 MHz                      |
| Bus topology           | Shared MOSI/MISO/SCK/CS     |
| Device addressing      | Hardware A0/A1/A2 + HAEN=1  |
| Supply voltage         | 3.3V                        |
| Master                 | ESP32 (HSPI or VSPI)        |

---

## Component Selection

### Master: ESP32-WROOM-32 (or any ESP32 variant)

- SPI2 (HSPI) or SPI3 (VSPI) available as general-purpose SPI master
- Max SPI clock: 80 MHz (IOMUX pins), 40 MHz (GPIO matrix)
- DMA support for efficient bulk transfers
- Running at 10 MHz SPI clock (MCP23S17 max) — well within ESP32 capability

### Slave: MCP23S17 (Microchip)

| Parameter                | Value                          |
|--------------------------|--------------------------------|
| I/O pins                 | 16 (Port A: GPA0–7, Port B: GPB0–7) |
| Max SPI clock            | 10 MHz                         |
| Supply voltage           | 1.8V–5.5V                      |
| Operating current        | ~1 mA typical                  |
| Address pins             | A0, A1, A2 (3 bits → 8 addresses) |
| Hardware address enable  | IOCON.HAEN bit (must be set)   |
| Interrupt outputs        | INTA, INTB (configurable)      |
| Package                  | SOIC-28, SSOP-28, QFN-28       |
| Unit cost                | ~$1.00–$1.80                   |

---

## Pin Mapping

### ESP32 Pinout (using VSPI — SPI3)

| ESP32 GPIO | Function | Connection        |
|------------|----------|-------------------|
| GPIO 18    | SCK      | All MCP23S17 SCK  |
| GPIO 23    | MOSI     | All MCP23S17 SI   |
| GPIO 19    | MISO     | All MCP23S17 SO   |
| GPIO 5     | CS       | All MCP23S17 /CS  |
| GPIO 4     | INT (optional) | OR'd interrupt line |

> Note: Using default VSPI IOMUX pins avoids GPIO matrix routing and allows
> higher clock speeds if needed in the future.

### MCP23S17 Pinout (SOIC-28)

| Pin | Name   | Function                     |
|-----|--------|------------------------------|
| 1   | GPB0   | Input channel 0 (port B)     |
| 2   | GPB1   | Input channel 1              |
| 3   | GPB2   | Input channel 2              |
| 4   | GPB3   | Input channel 3              |
| 5   | GPB4   | Input channel 4              |
| 6   | GPB5   | Input channel 5              |
| 7   | GPB6   | Input channel 6              |
| 8   | GPB7   | Input channel 7              |
| 9   | VDD    | 3.3V supply                  |
| 10  | VSS    | Ground                       |
| 11  | CS     | /CS (active low) — shared    |
| 12  | SCK    | SPI clock                    |
| 13  | SI     | SPI MOSI                     |
| 14  | SO     | SPI MISO                     |
| 15  | A0     | Address bit 0 (hardwired)    |
| 16  | A1     | Address bit 1 (hardwired)    |
| 17  | A2     | Address bit 2 (hardwired)    |
| 18  | INTA   | Interrupt output (Port A)    |
| 19  | INTB   | Interrupt output (Port B)    |
| 20  | /RESET | Reset (tie to VDD or RC)     |
| 21  | GPA0   | Input channel 8 (port A)     |
| 22  | GPA1   | Input channel 9              |
| 23  | GPA2   | Input channel 10             |
| 24  | GPA3   | Input channel 11             |
| 25  | GPA4   | Input channel 12             |
| 26  | GPA5   | Input channel 13             |
| 27  | GPA6   | Input channel 14             |
| 28  | GPA7   | Input channel 15             |

---

## Address Configuration

Each MCP23S17 gets a unique 3-bit address via A0, A1, A2 tied to VDD or GND:

| Device | A2 | A1 | A0 | Address | Channels    |
|--------|----|----|-----|---------|-------------|
| U1     | 0  | 0  | 0   | 0x40    | 0–15        |
| U2     | 0  | 0  | 1   | 0x42    | 16–31       |
| U3     | 0  | 1  | 0   | 0x44    | 32–47       |
| U4     | 0  | 1  | 1   | 0x46    | 48–63       |
| U5     | 1  | 0  | 0   | 0x48    | 64–79       |
| U6     | 1  | 0  | 1   | 0x4A    | 80–95       |
| U7     | 1  | 1  | 0   | 0x4C    | 96–111      |
| U8     | 1  | 1  | 1   | 0x4E    | 112–127     |

> The address byte format is: `0100 A2 A1 A0 R/W`
> (base opcode 0x40, R/W=0 for write, R/W=1 for read)

---

## SPI Protocol

### Transaction Format

Each SPI transaction to the MCP23S17 is 3 bytes (24 clocks):

```
Byte 0: Control byte — 0100 A2 A1 A0 R/W
Byte 1: Register address
Byte 2: Data (write) or dummy (read, device responds on MISO)
```

For reading both ports (16 bits) with sequential addressing (IOCON.SEQOP=0):

```
Byte 0: Control byte with R/W=1 (read)
Byte 1: 0x12 (GPIOA register address)
Byte 2: Device outputs Port A data on MISO
Byte 3: Device outputs Port B data on MISO (auto-incremented)
```

**Total: 4 bytes = 32 clock cycles per device read.**

### SPI Mode

- CPOL = 0, CPHA = 0 (SPI Mode 0)
- Data sampled on rising edge of SCK
- CS must be toggled between transactions (cannot be tied low)

---

## Timing Analysis

### Per-Transaction Timing at 10 MHz SCK

| Parameter              | Value      | Notes                              |
|------------------------|------------|------------------------------------|
| SCK period             | 100 ns     | 10 MHz                             |
| Bits per read (1 dev)  | 32 bits    | 1 control + 1 addr + 2 data bytes  |
| Clock time per device  | 3.2 µs     | 32 × 100 ns                        |
| CS setup time (tCSS)   | 50 ns min  | CS low to first SCK edge           |
| CS hold time (tCSH)    | 50 ns min  | Last SCK edge to CS high           |
| CS disable time (tCSD) | 50 ns min  | CS high to CS low (between xfers)  |
| CS overhead per xfer   | ~150 ns    | tCSS + tCSH + tCSD                 |
| Total per device       | ~3.35 µs   | 3.2 µs + 150 ns                    |
| **8 devices total**    | **~26.8 µs** | 8 × 3.35 µs                     |

### Achievable Sample Rate

| Metric                        | Value          |
|-------------------------------|----------------|
| Time for full 128-ch scan     | 26.8 µs        |
| Max theoretical scan rate     | **~37.3 kHz**  |
| Target scan rate              | 10 kHz         |
| Margin                        | 3.7× headroom  |
| Time budget per scan at 10kHz | 100 µs         |
| Remaining CPU time per cycle  | ~73 µs (free)  |

### ESP32 Software Overhead

| Factor                 | Estimated time | Notes                            |
|------------------------|----------------|----------------------------------|
| SPI driver setup/ISR   | ~2–5 µs       | Per transaction, ESP-IDF driver  |
| DMA setup (if used)    | ~1 µs         | One-time per batch               |
| GPIO CS toggle         | ~50 ns        | Direct register write            |
| Total software overhead| ~20–40 µs     | For 8 transactions               |
| **Realistic scan time**| **~50–70 µs** | Wire time + software overhead    |
| **Realistic scan rate**| **~14–20 kHz**| Comfortably exceeds 10 kHz       |

> Using DMA with pre-built transaction queues minimizes software overhead.
> Polling mode (no ISR) is even faster for short transactions.

---

## Bus Topology

```
                         3.3V
                          │
                    ┌─────┴─────┐
                    │  100nF ×8 │ (decoupling, one per device)
                    └─────┬─────┘
                          │
ESP32                     │
┌──────────┐              │
│ GPIO 18  │──── SCK ─────┼──── All 8× MCP23S17 pin 12
│ GPIO 23  │──── MOSI ────┼──── All 8× MCP23S17 pin 13
│ GPIO 19  │──── MISO ────┼──── All 8× MCP23S17 pin 14
│ GPIO  5  │──── /CS ─────┼──── All 8× MCP23S17 pin 11
│ GPIO  4  │──── INT ─────┼──── OR'd INTA+INTB (optional)
│          │              │
│     3.3V │──── VDD ─────┼──── All 8× MCP23S17 pin 9
│      GND │──── GND ─────┼──── All 8× MCP23S17 pin 10
└──────────┘              │
                          │
            A0/A1/A2 hardwired per device (see address table)
            /RESET tied to VDD via 10kΩ (or directly to VDD)
```

### Physical Layout Considerations

- Keep SPI traces < 20 cm total bus length at 10 MHz (wavelength ~30m, no issue)
- Place 100 nF ceramic cap as close to each VDD/VSS pair as possible
- Series termination resistor (33Ω) on SCK/MOSI at ESP32 end if ringing observed
- Pull-up on /CS (10kΩ to VDD) to prevent floating during ESP32 boot

---

## Initialization Sequence

```
1. Power-on reset (or assert /RESET low for >1µs, then release)
2. Wait 1 ms for device stabilization
3. For EACH device (or broadcast once since HAEN write is idempotent):
   a. Write IOCON register (0x0A): Set HAEN=1 (bit 3)
      - Byte sequence: [0x40, 0x0A, 0x08]
      - This enables hardware addressing on ALL devices simultaneously
4. For EACH device (now addressable individually):
   a. Write IODIRA (0x00): 0xFF (all Port A pins = input) — already default
   b. Write IODIRB (0x01): 0xFF (all Port B pins = input) — already default
   c. Write GPPUA (0x0C): 0xFF (enable pull-ups on Port A, if needed)
   d. Write GPPUB (0x0D): 0xFF (enable pull-ups on Port B, if needed)
   e. (Optional) Configure interrupts:
      - Write GPINTENA (0x04): 0xFF (enable IOC on Port A)
      - Write GPINTENB (0x05): 0xFF (enable IOC on Port B)
      - Write IOCON: Set MIRROR=1, ODR=1 (open-drain, mirrored interrupts)
```

---

## Polling Loop (Main Operation)

```
Every 100 µs (10 kHz timer):
  For device_addr in [0x40, 0x42, 0x44, 0x46, 0x48, 0x4A, 0x4C, 0x4E]:
    1. Assert /CS LOW
    2. Wait tCSS (50 ns — satisfied by GPIO write latency)
    3. Send: [device_addr | 0x01, 0x12]  (read command, start at GPIOA)
    4. Read: 2 bytes (Port A, Port B — sequential auto-increment)
    5. Wait tCSH (50 ns)
    6. Deassert /CS HIGH
    7. Wait tCSD (50 ns)
    8. Store 16-bit result in channel_data[device_index]
```

Result: `channel_data[8]` — array of 8× uint16_t = 128 bits of input state.

---

## Interrupt-Driven Alternative (Lower Latency)

Instead of polling at fixed rate, use interrupt-on-change:

```
1. Configure all devices for interrupt-on-change (GPINTEN = 0xFF)
2. Set IOCON: ODR=1 (open-drain), MIRROR=1 (INTA+INTB OR'd)
3. Wire all INTA/INTB to single ESP32 GPIO (open-drain OR — no external gate needed)
4. On ESP32 interrupt:
   a. Read INTCAP registers (0x10, 0x11) from all devices (captures pin state at interrupt time)
   b. Or read GPIO registers to get current state
   c. Interrupt clears automatically on register read
```

**Advantage:** Zero bus traffic when inputs are static. Responds within ~5 µs of a change.

**Disadvantage:** Worst case (all 128 inputs changing simultaneously) still takes ~27 µs — same as polling.

---

## Power Budget

| Component            | Current   | Qty | Total     |
|----------------------|-----------|-----|-----------|
| MCP23S17 (active)    | ~1 mA     | 8   | 8 mA      |
| MCP23S17 pull-ups    | ~0.15 mA/pin (if enabled, inputs low) | varies | 0–19 mA |
| ESP32 (active, WiFi off) | ~40 mA | 1   | 40 mA     |
| **Total (no pull-ups)**  |       |     | **~48 mA** |
| **Total (all pull-ups active, all inputs low)** | | | **~67 mA** |

---

## BOM (Per System)

| Qty | Part                    | Package  | Est. Cost |
|-----|-------------------------|----------|-----------|
| 1   | ESP32-WROOM-32          | Module   | $3.00     |
| 8   | MCP23S17-E/SO           | SOIC-28  | $1.20 ea  |
| 8   | 100 nF ceramic cap      | 0402/0603| $0.01 ea  |
| 1   | 10 kΩ resistor (CS pull-up) | 0402  | $0.01     |
| 8   | 10 kΩ resistor (/RESET pull-up) | 0402 | $0.01 ea |
|     | **Total**               |          | **~$12.80** |

> Optional: Add 33Ω series resistors on SCK/MOSI for signal integrity (~$0.02 each).

---

## Design Validation Checklist

- [ ] SPI clock ≤ 10 MHz (MCP23S17 max)
- [ ] HAEN enabled before individual addressing
- [ ] CS toggles between every transaction
- [ ] Each device has unique A2:A0 configuration
- [ ] 100 nF decoupling on every VDD pin
- [ ] /RESET not floating (pull-up or tied to VDD)
- [ ] Address pins not floating (tied to VDD or GND)
- [ ] Input pins have defined state (internal pull-ups or external conditioning)
- [ ] SPI bus length reasonable (< 30 cm recommended at 10 MHz)
- [ ] ESP32 boot state on GPIO 5 (/CS) does not cause glitches (use pull-up)

---

## Scaling Notes

- **More than 128 channels:** Add a second CS line. Another 8 devices on the same MOSI/MISO/SCK gives 256 channels. Only costs 1 more GPIO.
- **Faster sample rates:** Can't exceed 10 MHz SPI clock (device limit). For >37 kHz, use a second SPI bus (HSPI + VSPI) scanning in parallel → 256 channels at 37 kHz or 128 channels at 74 kHz.
- **Longer cable runs:** Use SPI line drivers (e.g., SN65HVD75) for runs >30 cm. Consider differential SPI or switch to RS-485 + UART for multi-meter distances.
