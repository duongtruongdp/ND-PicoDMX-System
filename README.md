# ND-PicoDMX-System

<p align="center">
  <strong>Budget-friendly, DIY 4-port Art-Net / sACN to DMX512 node built around the Raspberry Pi Pico.</strong><br>
  <strong>Node DMX512 4 cổng thân thiện ngân sách, dễ DIY, sử dụng Raspberry Pi Pico.</strong>
</p>

<p align="center">
  <img alt="Firmware" src="https://img.shields.io/badge/Firmware-v2.0.1-2563eb?style=for-the-badge">
  <img alt="Platform" src="https://img.shields.io/badge/Platform-RP2040-c51a4a?style=for-the-badge">
  <img alt="Ethernet" src="https://img.shields.io/badge/Ethernet-W5500-0f766e?style=for-the-badge">
  <img alt="Protocols" src="https://img.shields.io/badge/Protocols-Art--Net%20%7C%20sACN-f59e0b?style=for-the-badge">
  <img alt="Outputs" src="https://img.shields.io/badge/DMX%20Outputs-4-16a34a?style=for-the-badge">
</p>

<p align="center">
  <a href="#english">English</a> · <a href="#tieng-viet">Tiếng Việt</a>
</p>

> [!IMPORTANT]
> This README documents firmware **v2.0.1 (`2.0.1-temp-fix`)**. The node uses transmit-only, auto-direction RS485 modules. **RDM is not supported** by this hardware arrangement because no dedicated DE/RE control and no DMX receive path are used.
>
> README này mô tả firmware **v2.0.1 (`2.0.1-temp-fix`)**. Node sử dụng module RS485 auto-direction theo hướng chỉ phát. **Không hỗ trợ RDM** vì phần cứng không có điều khiển DE/RE riêng và không sử dụng đường nhận DMX.

---

<a id="english"></a>
# English Documentation

## Table of Contents

1. [Overview](#1-overview)
2. [DMX Fundamentals](#2-dmx-fundamentals)
3. [How DMX Works Inside the Firmware](#3-how-dmx-works-inside-the-firmware-firmware-internals)
4. [Key Features](#4-key-features)
5. [Hardware BOM](#5-hardware-bom)
6. [Pinout & Wiring](#6-pinout--wiring)
7. [Build & Compilation Guide](#7-build--compilation-guide)
8. [REST API Documentation](#8-rest-api-documentation)
9. [Network Defaults](#9-network-defaults)

---

## 1. Overview

**ND-PicoDMX-System** is a compact four-output network DMX node based on:

- **Raspberry Pi Pico / RP2040** for dual-core processing and PIO-based serial timing.
- **W5500 Ethernet** for wired Art-Net and sACN reception.
- **Four auto-direction TTL-to-RS485 modules** for four independent DMX512 outputs.
- A local, responsive **web dashboard** stored directly in firmware flash.

The project is designed to be:

- 💸 **Budget-friendly** — built from common modules instead of a custom PCB.
- 🧰 **DIY-friendly** — simple point-to-point wiring and Arduino IDE compilation.
- 🎛️ **Lighting-console friendly** — accepts both Art-Net and sACN/E1.31.
- 🌐 **Easy to configure** — live status, routing, diagnostics and output tests from a browser.
- ⚙️ **Timing-safe** — network/web work runs on Core 0, while DMX generation runs independently on Core 1 and PIO.

### Data path

```text
Lighting console / software
        │
        ├── Art-Net UDP 6454
        └── sACN UDP 5568
                │
             W5500
                │ SPI
             RP2040
        ┌───────┴────────┐
        │                │
   Core 0            Core 1 + PIO
Network / Web      DMX timing engine
        │                │
  Back buffers      Front buffers
        └──── spinlock ──┘
                │
       GP8 / GP9 / GP14 / GP15
                │
      4 × auto-direction RS485
                │
       4 × XLR-5 DMX outputs
```

> [!NOTE]
> The web dashboard is diagnostic and configuration software. It does not generate DMX timing in the browser. DMX continues to run on Core 1 even while the dashboard is open.

---

## 2. DMX Fundamentals

DMX512 is a unidirectional digital lighting-control protocol. One DMX universe carries up to **512 control slots**, commonly called channels.

### DMX electrical and serial basics

| Item | Value used by this firmware |
|---|---:|
| Physical layer | RS485 differential signalling |
| Baud rate | 250,000 bit/s |
| Serial format | 8 data bits, no parity, 2 stop bits — **8N2** |
| Start code | `0x00` for standard lighting data |
| Maximum channel slots | 512 |
| Bytes per full frame | 513: one start code + 512 channel bytes |
| Bit duration | 4 µs |
| Slot duration | Approximately 44 µs |
| Firmware BREAK | 105 µs |
| Firmware MAB | 14 µs |
| Output refresh period | 25 ms, approximately 40 Hz |

### Frame structure

```text
MARK / Idle HIGH
      │
      ├── BREAK: line LOW for 105 µs
      ├── MAB:   line HIGH for 14 µs
      ├── Start Code: 0x00
      ├── Channel 1
      ├── Channel 2
      ├── ...
      └── Channel 512

The sequence repeats approximately every 25 ms.
```

Each channel is an 8-bit value:

| Value | Percentage | Typical meaning |
|---:|---:|---|
| `0` | 0% | Off / minimum |
| `128` | About 50% | Mid-level |
| `255` | 100% | Full |

### DMX connector pinout

The node uses female XLR-5 panel connectors as DMX outputs.

| XLR-5 pin | Signal | Connection |
|---:|---|---|
| 1 | Common / shield | RS485 module GND / signal common |
| 2 | Data− | RS485 `B−` |
| 3 | Data+ | RS485 `A+` |
| 4 | Secondary data pair, unused | No connection |
| 5 | Secondary data pair, unused | No connection |

> [!WARNING]
> DMX termination belongs at the **last receiver on the cable**, normally 120 Ω between pins 2 and 3. Do not place permanent 120 Ω termination on every output unless the output stage was intentionally designed for it.

---

## 3. How DMX Works Inside the Firmware — Firmware Internals

### 3.1 Dual-core architecture

The firmware divides work between the two RP2040 cores:

| Processing block | Responsibility |
|---|---|
| **Core 0** | W5500 SPI, Art-Net, sACN, web server, REST API, EEPROM, telemetry and event log |
| **Core 1** | Four-port non-blocking DMX state machines |
| **PIO0 / PIO1** | Hardware-timed 250 kbit/s 8N2 payload transmission |

This separation prevents browser traffic or packet parsing from directly disturbing DMX bit timing.

### 3.2 PIO UART transmitter

Each DMX output is driven by a PIO state machine. The PIO clock divider produces a **1 MHz state-machine clock**, so one PIO cycle equals 1 µs. Four PIO cycles form one 4 µs DMX bit.

The compact PIO program performs:

1. `pull block` — wait for the next byte from the TX FIFO.
2. Drive the start bit LOW.
3. Shift eight data bits LSB-first to the output pin.
4. Drive two stop bits HIGH.
5. Return to MARK/idle while waiting for another byte.

PIO assignment:

| DMX port | GPIO | PIO block | State machine |
|---:|---:|---|---:|
| 1 | GP8 | PIO0 | SM0 |
| 2 | GP9 | PIO0 | SM1 |
| 3 | GP14 | PIO1 | SM0 |
| 4 | GP15 | PIO1 | SM1 |

### 3.3 Per-port DMX state machine

Every port cycles through four states:

```mermaid
stateDiagram-v2
    [*] --> IDLE
    IDLE --> BREAK: Frame time reached
    BREAK --> MAB: 105 µs elapsed
    MAB --> SENDING: 14 µs elapsed
    SENDING --> IDLE: 513 bytes queued
```

- **IDLE**: prepare the front buffer and wait for the 25 ms frame deadline.
- **BREAK**: switch the GPIO from PIO to normal GPIO and pull it LOW for 105 µs.
- **MAB**: pull the GPIO HIGH for 14 µs.
- **SENDING**: return ownership to PIO and feed start code plus 512 channel bytes into the TX FIFO.

The firmware feeds no more than 16 bytes per loop pass, keeping Core 1 fair across all four outputs.

### 3.4 Double buffering

Each port contains:

- `dmxBuffer[513]`: back buffer written by Core 0 from network packets.
- `activeDmxBuffer[513]`: front buffer transmitted by Core 1.
- `hasNewData`: flag indicating a new network frame is ready.

A hardware spinlock protects the buffer copy. Core 1 copies the back buffer only at a safe frame boundary, then keeps transmitting the front buffer continuously.

This means:

- A partial network update cannot tear a DMX frame.
- Output remains continuous when the lighting scene is static.
- Brief packet gaps do not immediately interrupt the DMX line.

### 3.5 Art-Net processing

The firmware listens on UDP port **6454** and supports:

- `ArtDmx` (`OpCode 0x5000`) for channel data.
- `ArtPoll` (`OpCode 0x2000`) and an `ArtPollReply` response for node discovery.

The received Art-Net Port-Address is compared with the configured port universe.

> [!TIP]
> Some controllers display Art-Net universes from 0 while others display them from 1. If the output appears one universe off, check the controller's Art-Net numbering convention.

### 3.6 sACN / E1.31 processing

The firmware listens on UDP port **5568** using:

- One unicast sACN socket.
- Up to four multicast sockets, one for each configured universe.

Multicast addresses follow:

```text
239.255.<Universe High Byte>.<Universe Low Byte>
```

The parser reads the universe, priority and DMX property values from the E1.31 packet. The start code is excluded from the incoming property list before copying channel data into the local 513-byte DMX buffer, whose index 0 remains the standard `0x00` start code.

### 3.7 Protocol selection and merge behavior

Each port can be configured as:

| Mode | Value | Behavior |
|---|---:|---|
| AUTO | `0` | Accept Art-Net or sACN |
| Art-Net only | `1` | Reject sACN for this port |
| sACN only | `2` | Reject Art-Net for this port |

Merge policy:

| Policy | Value | Current firmware behavior |
|---|---:|---|
| LTP | `0` | Latest accepted packet replaces the back buffer |
| HTP | `1` | Higher values overwrite lower values in the current back buffer |
| BLOCK | `2` | First active source IP is accepted; a second source is blocked while the first remains active |

For sACN, packets with lower priority than the currently active sACN source are ignored during the active-source window.

> [!NOTE]
> The current HTP implementation is intentionally lightweight and does not keep a separate 512-channel buffer for every sender. It should be treated as a simplified embedded HTP mode rather than a full multi-source merge engine.

### 3.8 Source health and conflict detection

- A source is marked inactive after **1 second** without a packet.
- A second sender IP detected within the active one-second window raises a conflict flag.
- The conflict flag clears two seconds after duplicate-source traffic stops.
- Dashboard input FPS counts received network packets.
- DMX output FPS is counted separately from actual Core 1 frame starts.

### 3.9 Local output test modes

The web dashboard can override each output locally:

| Mode | API value | Output |
|---|---:|---|
| NETWORK / STOP | `0` | Resume network buffer |
| BLACKOUT | `1` | All channels `0` |
| 50% | `2` | All channels `128` |
| FULL | `3` | All channels `255` |
| CHASE | `4` | One channel at `255`, advancing every 100 ms |

Network data continues updating the back buffer during a local test. When the test stops, the port returns to the latest received network data.

### 3.10 Temperature telemetry

Firmware v2.0.1 reads the RP2040 internal temperature sensor with `analogReadTemp(3.3f)`, discards the first conversion, averages valid samples and returns JSON `null` if no valid reading is available. The dashboard displays `SENSOR ERROR` instead of an invalid extreme value.

The internal sensor is useful for trend monitoring, not precision ambient measurement.

---

## 4. Key Features

| Feature | Description |
|---|---|
| 🎚️ Four independent DMX outputs | GP8, GP9, GP14 and GP15 drive four RS485 transmitters |
| 🌐 Art-Net reception | ArtDmx input and ArtPollReply discovery on UDP 6454 |
| 📡 sACN / E1.31 reception | Unicast plus per-universe multicast on UDP 5568 |
| ⚙️ RP2040 dual-core architecture | Core 0 handles network/web; Core 1 handles DMX |
| ⏱️ PIO-timed DMX | Hardware-timed 250 kbit/s, 8N2 payload |
| 🔁 Continuous 40 Hz output | Repeats the current frame even when the scene does not change |
| 🧠 Double buffering | Prevents torn frames between network and output cores |
| 🔀 Per-port protocol selection | AUTO, Art-Net only or sACN only |
| 🧩 Routing modes | Sequential universes or manual universe per port |
| ⚔️ Conflict handling | LTP, simplified HTP or first-source BLOCK |
| 🚦 Source diagnostics | Sender IP, packet age, input FPS, output FPS and sACN priority |
| 🧪 Output test tools | Blackout, 50%, full and channel chase |
| 🖥️ Local web dashboard | No CDN, no external fonts and no Internet requirement |
| 📦 Flash-resident UI | Dashboard is stored as one static document in firmware |
| 🧱 Chunked web transfer | 512-byte Ethernet writes with UDP service between chunks |
| 💾 EEPROM configuration | Routing, universe, protocol, enable state and merge policy persist |
| 📝 Event log | Ten-entry circular RAM log |
| 🌡️ MCU temperature | Filtered RP2040 internal temperature telemetry |
| 🛡️ Watchdog | Hardware watchdog and Core 1 heartbeat monitoring |

### Current limitations

- Static IPv4 configuration only; no DHCP in v2.0.1.
- No RDM because the hardware is transmit-only and has no dedicated DE/RE or RX path.
- No galvanic isolation unless the selected RS485 modules specifically provide it.
- The web API changes state through HTTP GET requests for embedded simplicity. Keep the node on a trusted lighting-control LAN.
- Temperature is an internal RP2040 estimate, not calibrated enclosure temperature.

---

## 5. Hardware BOM

| Qty. | Component | Minimum recommendation | Purpose / notes |
|---:|---|---|---|
| 1 | [Raspberry Pi Pico](https://vi.aliexpress.com/item/1005012706861207.html)| Standard RP2040 Pico, 2 MB flash | Main processor |
| 1 | [Pico expansion board](https://vi.aliexpress.com/item/1005005292085342.html) | 7–12 V DC input with regulated rails | Accepts the 12 V node branch and powers the Pico system |
| 1 | [W5500 Ethernet module](https://vi.aliexpress.com/item/1005012641099047.html) | SPI module, 3.3 V logic | Wired network interface |
| 4 | [Auto-direction TTL-to-RS485 modules](https://vi.aliexpress.com/item/1005003056616563.html) | 250 kbit/s or faster, compatible with 3.3 V logic | One transmitter per DMX output |
| 4 | [XLR-5 female panel connectors](https://vi.aliexpress.com/item/1005004974465236.html) | Metal or quality plastic chassis type | DMX output connectors |
| 1 | 12 V DC router | LAN router with matching supply voltage | Connects node and lighting controller |
| 1 | [12 V DC power supply](https://vi.aliexpress.com/item/1005010390772322.html) | Sized for router + node; typically 2 A or greater after calculation | Main system supply |
| 1 | [Two-way fused power distribution](https://vi.aliexpress.com/item/1005009381318344.html) | Separate fuse for node and router branches | Splits the 12 V input safely |
| 1 | [Ethernet cable](https://vi.aliexpress.com/item/1005003335746833.html) | Cat5e or Cat6 | W5500 to router |
| 1 set | [Hook-up wire and connectors](https://vi.aliexpress.com/item/1005002984683377.html) | Short TTL/SPI wires, suitable gauge power wire | Internal wiring |
| 1 | 3D Print Enclosure | Ventilated metal/plastic project enclosure | Mechanical protection |
| Optional | External status LED + resistor | 220 Ω–1 kΩ series resistor | Use a genuinely exposed GPIO; see GP23 warning below |
| Optional | 100 nF decoupling capacitors | One close to each module supply | Local high-frequency decoupling |
| Optional | Bulk capacitor | 220–470 µF on the regulated rail | Helps absorb load transients |
| Optional | DMX terminator | 120 Ω between XLR pins 2 and 3 | Use at the end of the DMX cable, not automatically at every node output |


> [!CAUTION]
> Confirm the actual supply requirements printed on your W5500 and RS485 modules. Module boards with similar appearance may use different regulators, transceivers and VCC ranges. RP2040 GPIO is 3.3 V logic and is not 5 V tolerant.

### Suggested 12 V power architecture

```text
12 V DC input
      │
      ├── Fuse A ──> Pico expansion-board 7–12 V input
      │                ├── Raspberry Pi Pico
      │                ├── W5500 module
      │                └── Four RS485 modules
      │
      └── Fuse B ──> 12 V router input
```

Use a supply with sufficient current for both branches and maintain solid ground connections on the low-voltage node side.

---

## 6. Pinout & Wiring

<p align="center">
  <a href="[https://github.com/duongtruongdp/ND-PicoDMX-System]"><img src="images/ND DMX SYSTEM WIRING DIAGRAM.jpg" alt="ND DMX NODE 4U Wiring Diagram" width="100%"></a>
</p>

### W5500 ↔ Raspberry Pi Pico

| W5500 signal | Pico GPIO / rail | Firmware symbol | Function |
|---|---|---|---|
| MISO | GP16 | `PIN_MISO` | SPI data from W5500 to Pico |
| CS / SCS | GP17 | `PIN_CS` | W5500 chip select |
| SCK | GP18 | `PIN_SCK` | SPI clock |
| MOSI | GP19 | `PIN_MOSI` | SPI data from Pico to W5500 |
| RST | GP20 | `PIN_RST` | Hardware reset, active LOW |
| VCC | Module-dependent regulated rail | — | Follow the W5500 board marking |
| GND | Common GND | — | Logic and power reference |
| RJ45 | Router LAN port | — | Ethernet network connection |

### DMX output GPIO ↔ auto-direction RS485 modules

This table follows the tested project wiring: the Pico output is connected to the module pin marked **TXD**, while the module **RXD** pin is unused.

| DMX output | Pico GPIO | RS485 TTL pin | Other RS485 TTL connections | Function |
|---:|---:|---|---|---|
| Port 1 | GP8 | TXD | VCC → 3.3 V, GND → GND, RXD → NC | Universe output 1 |
| Port 2 | GP9 | TXD | VCC → 3.3 V, GND → GND, RXD → NC | Universe output 2 |
| Port 3 | GP14 | TXD | VCC → 3.3 V, GND → GND, RXD → NC | Universe output 3 |
| Port 4 | GP15 | TXD | VCC → 3.3 V, GND → GND, RXD → NC | Universe output 4 |

> [!WARNING]
> TTL pin naming is not consistent across inexpensive auto-direction RS485 modules. On some boards, the MCU transmit input may be labelled `RXD`, `DI` or `DIN` instead of `TXD`. This project documents the module revision that was tested successfully with **Pico GPIO → module TXD**. Verify the silkscreen or vendor diagram for your exact module before powering it.

### RS485 modules ↔ XLR-5 outputs

Repeat for all four ports:

| RS485 screw terminal | XLR-5 pin | DMX signal |
|---|---:|---|
| GND | 1 | Common / shield |
| B− | 2 | Data− |
| A+ | 3 | Data+ |
| — | 4 | Unused |
| — | 5 | Unused |

### Power wiring

| From | To | Function |
|---|---|---|
| 12 V supply positive | Two-way fused distribution input | Main positive feed |
| Distribution branch A | Expansion-board 7–12 V input | Powers node electronics |
| Distribution branch B | Router 12 V input | Powers router |
| 12 V supply negative | Expansion-board negative and router negative | Common low-voltage return |
| Expansion-board regulated 3.3 V | Four RS485 VCC pins, if supported by modules | RS485 logic/transceiver power |
| Expansion-board regulated rail | W5500 VCC according to module marking | Ethernet module power |

### Status pin warning: GP23

The current firmware defines:

```cpp
#define HARDWARE_LED_PIN 23
```

and toggles that pin every 300 ms. On a **standard Raspberry Pi Pico**, GP23 is an internal board signal connected to the SMPS power-save control and is not available on the normal 40-pin header. It should not be treated as an external LED pin.

For a standard Pico, choose one of these options before final hardware assembly:

1. Change `HARDWARE_LED_PIN` to **GP25** to use the Pico's onboard LED.
2. Change it to another unused, exposed GPIO and connect an external LED through a current-limiting resistor.
3. Disable the heartbeat LED code if all exposed GPIOs are required.

The current blink pattern is a simple firmware heartbeat: approximately 300 ms ON and 300 ms OFF. It indicates that Core 0 is looping; it does **not** directly indicate DMX packet activity.

---

## 7. Build & Compilation Guide

### 7.1 Required software

- Arduino IDE 2.x.
- Raspberry Pi Pico/RP2040 Arduino core by Earle F. Philhower.
- Arduino `Ethernet` library with W5500 support.
- The firmware `.ino` file from this repository.

No external DMX library is required. DMX generation is implemented directly with RP2040 PIO and the dual-core runtime.

### 7.2 Install the RP2040 board package

In Arduino IDE:

1. Open **Arduino IDE → Settings / Preferences**.
2. Add this Boards Manager URL:

   ```text
   https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
   ```

3. Open **Boards Manager**.
4. Search for **Raspberry Pi Pico/RP2040/RP2350**.
5. Install the package by **Earle F. Philhower**.

### 7.3 Install the Ethernet library

Open **Library Manager**, search for **Ethernet**, and install the Arduino Ethernet library.

### 7.4 Recommended Arduino settings

For a standard Raspberry Pi Pico:

| Setting | Value |
|---|---|
| Board | Raspberry Pi Pico |
| CPU Speed | 133 MHz |
| Flash Size | 2 MB, no filesystem required |
| USB Stack | Pico SDK |
| Optimize | Small (`-Os`) |
| Serial Monitor | 115200 baud |

> [!IMPORTANT]
> Do not select a 16 MB flash layout for a standard 2 MB Raspberry Pi Pico. EEPROM emulation depends on the configured flash layout.

### 7.5 Compile and upload

1. Place the firmware in a folder with the same base name as the `.ino` file.
2. Connect the Pico while holding **BOOTSEL** if no serial port is available.
3. Wait for the `RPI-RP2` drive to appear.
4. Click **Upload** in Arduino IDE.
5. After the UF2 copy completes, the drive disappears and the Pico reboots.
6. Open Serial Monitor at 115200 baud to inspect boot checkpoints.
7. Connect W5500 to the router and browse to:

   ```text
   http://10.10.10.10/
   ```
<p align="center">
  <a href="[https://github.com/duongtruongdp/ND-PicoDMX-System]"><img src="images/WEB DASHBOARD - 01.png" alt="Live Dashboard" width="100%"></a>
</p>
<p align="center">
  <a href="[https://github.com/duongtruongdp/ND-PicoDMX-System]"><img src="images/WEB DASHBOARD - 02.png" alt="Live Dashboard" width="100%"></a>
</p>
<p align="center">
  <a href="[https://github.com/duongtruongdp/ND-PicoDMX-System]"><img src="images/WEB DASHBOARD - 03.png" alt="Live Dashboard" width="100%"></a>
</p>

### 7.6 First test checklist

- [ ] W5500 link LEDs illuminate.
- [ ] `http://10.10.10.10/api/status` returns JSON.
- [ ] Dashboard reports `SYSTEM ONLINE`.
- [ ] Test mode `FULL` drives a connected fixture.
- [ ] Art-Net or sACN source appears with sender IP and FPS.
- [ ] All four ports report approximately 40 Hz DMX output.
- [ ] Temperature shows a reasonable value or `SENSOR ERROR`, never an extreme invalid number.

---

## 8. REST API Documentation

The HTTP server listens on TCP port **80**. The current firmware uses lightweight GET endpoints to simplify the embedded web stack.

> [!WARNING]
> There is no authentication or TLS. Operate the node only on a trusted, isolated lighting-control network.

### 8.1 `GET /api/status`

Returns current device, network, DMX-port and event-log telemetry.

Example:

```bash
curl http://10.10.10.10/api/status
```

Representative response:

```json
{
  "model": "ND DMX NODE 4U",
  "firmware": "2.0.1-temp-fix",
  "hardware": "RP2040 + W5500 + 4x RS485",
  "build": "Aug 01 2026 18:00:00",
  "mac": "02:4E:44:34:55:01",
  "ip": "10.10.10.10",
  "link": true,
  "mode": 0,
  "start": 1,
  "merge": 0,
  "uptime": 3600,
  "c0": 4200,
  "c1": 120000,
  "net": 850,
  "packets": 144000,
  "temp": 41.7,
  "ports": [
    {
      "port": 1,
      "en": 1,
      "pmode": 0,
      "universe": 1,
      "active": true,
      "conflict": false,
      "fps": 40.0,
      "dmxFps": 40.0,
      "used": 128,
      "age": 8,
      "prio": 100,
      "test": 0,
      "proto": "Art-Net",
      "sender": "10.10.10.20",
      "c_ip": "0.0.0.0"
    }
  ],
  "events": [
    {"t": 1000, "m": "System boot complete"}
  ]
}
```

`temp` is either a numeric Celsius value or JSON `null` when the internal sensor reading is invalid.

#### Status field reference

| Field | Meaning |
|---|---|
| `model` | Device model string |
| `firmware` | Firmware version |
| `hardware` | Hardware description |
| `build` | Compile date and time |
| `mac` | Ethernet MAC address |
| `ip` | Current local IPv4 address |
| `link` | W5500 physical link state |
| `mode` | Routing mode: `0` sequential, `1` manual |
| `start` | Sequential start universe |
| `merge` | `0` LTP, `1` HTP, `2` BLOCK |
| `uptime` | Seconds since boot |
| `c0`, `c1` | Core loop counters per reporting interval |
| `net` | Maximum measured network service time in µs |
| `packets` | Total accepted Art-Net/sACN packets |
| `temp` | RP2040 internal temperature in °C or `null` |
| `ports` | Array of four output status objects |
| `events` | Newest-first circular event log |

Per-port fields:

| Field | Meaning |
|---|---|
| `port` | Human-readable port number, 1–4 |
| `en` | Port enabled state, 0 or 1 |
| `pmode` | Protocol filter: 0 AUTO, 1 Art-Net, 2 sACN |
| `universe` | Routed universe |
| `active` | Network source received within one second |
| `conflict` | A second source IP was detected |
| `fps` | Accepted network packet FPS |
| `dmxFps` | Physical DMX frame-start FPS |
| `used` | Highest non-zero channel index, not a count of all non-zero channels |
| `age` | Milliseconds since last accepted packet |
| `prio` | Current sACN priority, or fixed Art-Net priority value |
| `test` | Local test mode 0–4 |
| `proto` | Active protocol name |
| `sender` | Current source IPv4 address |
| `c_ip` | Conflicting source IPv4 address |

### 8.2 `GET /api/test`

Changes local output-test mode.

Parameters:

| Parameter | Range | Meaning |
|---|---:|---|
| `p` | `0`–`3` | Zero-based output index |
| `m` | `0`–`4` | Test mode |

Test modes:

| `m` | Mode |
|---:|---|
| 0 | Stop test / resume network |
| 1 | Blackout |
| 2 | All channels 50% |
| 3 | All channels 100% |
| 4 | Channel chase |

Examples:

```bash
# Full output on physical Port 1
curl "http://10.10.10.10/api/test?p=0&m=3"

# Stop test and return Port 1 to network control
curl "http://10.10.10.10/api/test?p=0&m=0"
```

Success response:

```json
{"ok":true,"message":"Test mode updated"}
```

### 8.3 `GET /api/config`

Updates routing and persistent port configuration.

All parameters must be present because the firmware validates every port before saving.

| Parameter | Values | Meaning |
|---|---|---|
| `mode` | `0`, `1` | Sequential or manual routing |
| `start` | `1`–`63996` | Start universe for sequential mode |
| `merge` | `0`, `1`, `2` | LTP, HTP or BLOCK |
| `u0`…`u3` | `1`–`63999` | Manual universe for ports 1–4 |
| `e0`…`e3` | `0`, `1` | Disable or enable ports 1–4 |
| `p0`…`p3` | `0`, `1`, `2` | AUTO, Art-Net only or sACN only |

Example — sequential Universes 1–4, all ports enabled, AUTO protocol and LTP:

```bash
curl "http://10.10.10.10/api/config?mode=0&start=1&merge=0&u0=1&e0=1&p0=0&u1=2&e1=1&p1=0&u2=3&e2=1&p2=0&u3=4&e3=1&p3=0"
```

Success response:

```json
{"ok":true,"message":"Configuration saved"}
```

Saving configuration writes EEPROM and resets current network-source state for all ports.

### 8.4 Legacy and page routes

| Route | Behavior |
|---|---|
| `GET /` | Local dashboard |
| `GET /index.html` | Local dashboard |
| `GET /set?...` | Legacy configuration route; redirects to `/` after success |
| `GET /favicon.ico` | Returns HTTP 204 |
| Other routes | HTTP 404 `Not found` |

---

## 9. Network Defaults

### IPv4 defaults

| Setting | Default |
|---|---|
| Node IP | `10.10.10.10` |
| Subnet mask | `255.255.255.0` (`/24`) |
| Gateway | `10.10.10.1` |
| DNS server | `10.10.10.1` |
| MAC address | `02:4E:44:34:55:01` |
| HTTP dashboard | TCP 80 |
| Art-Net | UDP 6454 |
| sACN / E1.31 | UDP 5568 |
| Address mode | Static IPv4 |

Suggested computer settings for direct or router-based testing:

```text
IP address:  10.10.10.20
Subnet mask: 255.255.255.0
Gateway:     10.10.10.1
```

### Routing defaults after invalid or blank EEPROM

| Setting | Default |
|---|---|
| Routing mode | Sequential |
| Start universe | 1 |
| Port 1 universe | 1 |
| Port 2 universe | 2 |
| Port 3 universe | 3 |
| Port 4 universe | 4 |
| Port enabled | All ON |
| Protocol filter | AUTO on all ports |
| Merge policy | LTP |

### sACN multicast examples

| Universe | Multicast group |
|---:|---|
| 1 | `239.255.0.1` |
| 2 | `239.255.0.2` |
| 256 | `239.255.1.0` |

---

## References and inspiration

- Raspberry Pi Pico documentation: [Raspberry Pi documentation](https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html)
- Arduino-Pico core documentation: [arduino-pico.readthedocs.io](https://arduino-pico.readthedocs.io/)

---

<a id="tieng-viet"></a>
# Tài Liệu Tiếng Việt

## Mục Lục

1. [Tổng Quan](#1-tổng-quan)
2. [Kiến Thức Nền Tảng Về DMX](#2-kiến-thức-nền-tảng-về-dmx)
3. [Cách Thức DMX Hoạt Động Trong Mã Nguồn](#3-cách-thức-dmx-hoạt-động-trong-mã-nguồn-firmware-internals)
4. [Tính Năng Nổi Bật](#4-tính-năng-nổi-bật)
5. [Danh Sách Phần Cứng — Hardware BOM](#5-danh-sách-phần-cứng--hardware-bom)
6. [Sơ Đồ Chân Kết Nối — Pinout & Wiring](#6-sơ-đồ-chân-kết-nối--pinout--wiring)
7. [Hướng Dẫn Build & Biên Dịch](#7-hướng-dẫn-build--biên-dịch)
8. [Tài Liệu REST API](#8-tài-liệu-rest-api)
9. [Cấu Hình Mạng Mặc Định](#9-cấu-hình-mạng-mặc-định)

---

## 1. Tổng Quan

**ND-PicoDMX-System** là bộ chuyển đổi Art-Net/sACN sang DMX512 gồm bốn cổng độc lập, được xây dựng từ:

- **Raspberry Pi Pico / RP2040** để xử lý hai nhân và tạo timing DMX bằng PIO.
- **W5500 Ethernet** để nhận dữ liệu Art-Net và sACN qua mạng LAN có dây.
- **Bốn module TTL-to-RS485 auto-direction** cho bốn đường DMX output.
- **Web dashboard nội bộ** được lưu trực tiếp trong flash của firmware.

Mục tiêu thiết kế:

- 💸 **Tiết kiệm chi phí** — sử dụng module phổ biến, không bắt buộc PCB tùy chỉnh.
- 🧰 **Dễ DIY** — đấu dây trực tiếp và biên dịch bằng Arduino IDE.
- 🎛️ **Tương thích phần mềm ánh sáng** — nhận Art-Net và sACN/E1.31.
- 🌐 **Dễ cấu hình** — theo dõi trạng thái, universe, diagnostics và test output trên trình duyệt.
- ⚙️ **Ưu tiên độ ổn định DMX** — Core 0 xử lý mạng/web; Core 1 và PIO độc lập xuất DMX.

### Luồng dữ liệu

```text
Console / phần mềm ánh sáng
        │
        ├── Art-Net UDP 6454
        └── sACN UDP 5568
                │
             W5500
                │ SPI
             RP2040
        ┌───────┴────────┐
        │                │
   Core 0            Core 1 + PIO
  Mạng / Web        Bộ máy timing DMX
        │                │
   Back buffer       Front buffer
        └──── spinlock ──┘
                │
       GP8 / GP9 / GP14 / GP15
                │
      4 × module RS485 auto-direction
                │
       4 × jack XLR-5 DMX output
```

> [!NOTE]
> Web dashboard chỉ làm nhiệm vụ cấu hình và chẩn đoán. Trình duyệt không tạo timing DMX. Core 1 vẫn tiếp tục phát DMX khi người dùng đang mở dashboard.

---

## 2. Kiến Thức Nền Tảng Về DMX

DMX512 là giao thức điều khiển ánh sáng kỹ thuật số một chiều. Một DMX universe có tối đa **512 slot điều khiển**, thường được gọi là channel.

### Thông số điện và serial

| Hạng mục | Giá trị firmware sử dụng |
|---|---:|
| Lớp vật lý | Tín hiệu vi sai RS485 |
| Baud rate | 250.000 bit/s |
| Định dạng serial | 8 data bit, không parity, 2 stop bit — **8N2** |
| Start code | `0x00` cho dữ liệu ánh sáng tiêu chuẩn |
| Số channel tối đa | 512 |
| Số byte mỗi frame đầy đủ | 513: một start code + 512 channel |
| Thời gian mỗi bit | 4 µs |
| Thời gian một slot | Khoảng 44 µs |
| BREAK trong firmware | 105 µs |
| MAB trong firmware | 14 µs |
| Chu kỳ refresh output | 25 ms, khoảng 40 Hz |

### Cấu trúc một frame DMX

```text
MARK / trạng thái nghỉ HIGH
      │
      ├── BREAK: kéo LOW trong 105 µs
      ├── MAB:   kéo HIGH trong 14 µs
      ├── Start Code: 0x00
      ├── Channel 1
      ├── Channel 2
      ├── ...
      └── Channel 512

Toàn bộ chuỗi được lặp lại khoảng mỗi 25 ms.
```

Mỗi channel là một giá trị 8 bit:

| Giá trị | Phần trăm | Ý nghĩa thường gặp |
|---:|---:|---|
| `0` | 0% | Tắt / mức thấp nhất |
| `128` | Khoảng 50% | Mức trung bình |
| `255` | 100% | Mức tối đa |

### Pin jack DMX XLR-5

Node sử dụng jack XLR-5 cái gắn panel làm DMX output.

| Chân XLR-5 | Tín hiệu | Kết nối |
|---:|---|---|
| 1 | Common / shield | GND / signal common của module RS485 |
| 2 | Data− | `B−` của module RS485 |
| 3 | Data+ | `A+` của module RS485 |
| 4 | Cặp dữ liệu phụ, không dùng | Không nối |
| 5 | Cặp dữ liệu phụ, không dùng | Không nối |

> [!WARNING]
> Điện trở termination DMX phải nằm ở **thiết bị nhận cuối cùng trên đường dây**, thông thường 120 Ω giữa chân 2 và 3. Không gắn cố định 120 Ω trên mọi cổng output nếu tầng output không được thiết kế riêng cho việc đó.

---

## 3. Cách Thức DMX Hoạt Động Trong Mã Nguồn — Firmware Internals

### 3.1 Kiến trúc hai nhân

Firmware chia nhiệm vụ như sau:

| Khối xử lý | Nhiệm vụ |
|---|---|
| **Core 0** | SPI W5500, Art-Net, sACN, web server, REST API, EEPROM, telemetry và event log |
| **Core 1** | Bốn state machine DMX không chặn luồng |
| **PIO0 / PIO1** | Truyền payload 250 kbit/s 8N2 với timing phần cứng |

Việc tách riêng giúp tải web và xử lý packet không trực tiếp làm sai timing bit DMX.

### 3.2 Bộ phát UART bằng PIO

Mỗi DMX output được điều khiển bởi một PIO state machine. Clock divider tạo clock PIO **1 MHz**, tương đương một chu kỳ PIO là 1 µs. Bốn chu kỳ PIO tạo thành một bit DMX dài 4 µs.

Chương trình PIO thực hiện:

1. `pull block` — chờ byte tiếp theo từ TX FIFO.
2. Phát start bit LOW.
3. Dịch tám data bit theo LSB-first ra GPIO.
4. Phát hai stop bit HIGH.
5. Giữ MARK/HIGH khi chờ byte tiếp theo.

Phân bổ PIO:

| Cổng DMX | GPIO | PIO | State machine |
|---:|---:|---|---:|
| 1 | GP8 | PIO0 | SM0 |
| 2 | GP9 | PIO0 | SM1 |
| 3 | GP14 | PIO1 | SM0 |
| 4 | GP15 | PIO1 | SM1 |

### 3.3 State machine của từng cổng

```mermaid
stateDiagram-v2
    [*] --> IDLE
    IDLE --> BREAK: Đến thời điểm frame mới
    BREAK --> MAB: Đủ 105 µs
    MAB --> SENDING: Đủ 14 µs
    SENDING --> IDLE: Đã nạp 513 byte
```

- **IDLE**: chuẩn bị front buffer và chờ mốc 25 ms.
- **BREAK**: chuyển GPIO khỏi PIO, kéo LOW trong 105 µs.
- **MAB**: kéo GPIO HIGH trong 14 µs.
- **SENDING**: trả GPIO cho PIO và nạp start code cùng 512 channel vào TX FIFO.

Mỗi lượt quét chỉ nạp tối đa 16 byte để Core 1 phục vụ công bằng cả bốn cổng.

### 3.4 Double buffering

Mỗi port có:

- `dmxBuffer[513]`: back buffer do Core 0 ghi từ packet mạng.
- `activeDmxBuffer[513]`: front buffer do Core 1 phát.
- `hasNewData`: cờ báo có frame mạng mới.

Spinlock phần cứng bảo vệ thao tác copy. Core 1 chỉ copy dữ liệu tại ranh giới frame an toàn rồi tiếp tục phát front buffer liên tục.

Kết quả:

- Không xuất hiện frame bị xé khi Core 0 đang cập nhật dữ liệu.
- Cảnh tĩnh vẫn được refresh liên tục.
- Mất packet ngắn không làm đường DMX tắt ngay lập tức.

### 3.5 Xử lý Art-Net

Firmware nghe UDP port **6454** và xử lý:

- `ArtDmx` (`OpCode 0x5000`) để nhận channel.
- `ArtPoll` (`OpCode 0x2000`) và trả `ArtPollReply` để phần mềm dò node.

Port-Address nhận được sẽ được so sánh với universe đã cấu hình của cổng.

> [!TIP]
> Có phần mềm hiển thị Art-Net universe từ 0, có phần mềm hiển thị từ 1. Nếu tín hiệu lệch một universe, hãy kiểm tra quy ước đánh số Art-Net của phần mềm điều khiển.

### 3.6 Xử lý sACN / E1.31

Firmware nghe UDP port **5568** bằng:

- Một socket sACN unicast.
- Tối đa bốn socket multicast, tương ứng universe của từng port.

Địa chỉ multicast:

```text
239.255.<Byte cao của Universe>.<Byte thấp của Universe>
```

Parser đọc universe, priority và property values. Start code trong packet được loại ra trước khi channel được copy vào buffer DMX 513 byte; byte index 0 của buffer vẫn là start code `0x00`.

### 3.7 Chọn protocol và merge

Mỗi port có các chế độ:

| Chế độ | Giá trị | Hành vi |
|---|---:|---|
| AUTO | `0` | Nhận Art-Net hoặc sACN |
| Chỉ Art-Net | `1` | Bỏ packet sACN ở port này |
| Chỉ sACN | `2` | Bỏ packet Art-Net ở port này |

Merge policy:

| Policy | Giá trị | Hành vi hiện tại |
|---|---:|---|
| LTP | `0` | Packet hợp lệ mới nhất thay thế back buffer |
| HTP | `1` | Giá trị cao hơn ghi đè giá trị thấp hơn trong back buffer hiện tại |
| BLOCK | `2` | Giữ IP nguồn đầu tiên; chặn IP thứ hai khi nguồn đầu vẫn active |

Với sACN, packet có priority thấp hơn nguồn sACN đang active sẽ bị bỏ trong cửa sổ nguồn active.

> [!NOTE]
> HTP hiện tại là phiên bản nhẹ dành cho embedded và không lưu riêng buffer 512 channel cho từng sender. Không nên xem đây là bộ merge đa nguồn đầy đủ như trên console chuyên nghiệp.

### 3.8 Theo dõi nguồn và conflict

- Port chuyển inactive sau **1 giây** không có packet.
- IP nguồn thứ hai xuất hiện trong cửa sổ một giây sẽ kích hoạt conflict.
- Conflict tự xóa sau hai giây không còn dữ liệu trùng nguồn.
- Input FPS là tốc độ packet mạng được chấp nhận.
- DMX Output FPS được đếm riêng theo số lần Core 1 bắt đầu frame thực tế.

### 3.9 Output Test cục bộ

| Chế độ | Giá trị API | Dữ liệu output |
|---|---:|---|
| NETWORK / STOP | `0` | Trở lại dữ liệu mạng |
| BLACKOUT | `1` | Toàn bộ channel bằng `0` |
| 50% | `2` | Toàn bộ channel bằng `128` |
| FULL | `3` | Toàn bộ channel bằng `255` |
| CHASE | `4` | Một channel bằng `255`, chuyển channel mỗi 100 ms |

Trong lúc test, dữ liệu mạng vẫn cập nhật back buffer. Khi dừng test, cổng trở lại dữ liệu mạng mới nhất.

### 3.10 Telemetry nhiệt độ

Firmware v2.0.1 đọc cảm biến nhiệt nội bộ RP2040 bằng `analogReadTemp(3.3f)`, bỏ mẫu đầu tiên, lấy trung bình các mẫu hợp lệ và trả JSON `null` nếu không đọc được. Dashboard sẽ hiện `SENSOR ERROR` thay vì một giá trị cực đoan sai.

Cảm biến này phù hợp để theo dõi xu hướng nhiệt, không phải cảm biến nhiệt độ môi trường chính xác.

---

## 4. Tính Năng Nổi Bật

| Tính năng | Mô tả |
|---|---|
| 🎚️ Bốn DMX output độc lập | GP8, GP9, GP14 và GP15 điều khiển bốn module RS485 |
| 🌐 Nhận Art-Net | ArtDmx và ArtPollReply trên UDP 6454 |
| 📡 Nhận sACN / E1.31 | Unicast và multicast theo universe trên UDP 5568 |
| ⚙️ Kiến trúc hai nhân RP2040 | Core 0 xử lý mạng/web; Core 1 xử lý DMX |
| ⏱️ Timing DMX bằng PIO | Payload 250 kbit/s, 8N2 bằng phần cứng |
| 🔁 Output liên tục khoảng 40 Hz | Vẫn phát frame khi cảnh không thay đổi |
| 🧠 Double buffering | Tránh frame bị xé giữa hai nhân |
| 🔀 Chọn protocol theo port | AUTO, chỉ Art-Net hoặc chỉ sACN |
| 🧩 Hai chế độ routing | Universe tuần tự hoặc universe riêng từng cổng |
| ⚔️ Xử lý conflict | LTP, HTP đơn giản hoặc BLOCK nguồn đầu tiên |
| 🚦 Diagnostics nguồn | IP sender, packet age, input FPS, output FPS và sACN priority |
| 🧪 Công cụ test output | Blackout, 50%, full và channel chase |
| 🖥️ Web dashboard nội bộ | Không cần CDN, font online hoặc Internet |
| 📦 UI nằm trong flash | Dashboard là một tài liệu tĩnh trong firmware |
| 🧱 Truyền web theo block | Block 512 byte, xử lý UDP xen kẽ |
| 💾 Lưu cấu hình EEPROM | Lưu routing, universe, protocol, enable và merge |
| 📝 Event log | Circular log cố định 10 sự kiện trong RAM |
| 🌡️ Nhiệt độ MCU | Lọc dữ liệu cảm biến nhiệt nội bộ RP2040 |
| 🛡️ Watchdog | Watchdog phần cứng và heartbeat của Core 1 |

### Giới hạn hiện tại

- Chỉ dùng IPv4 tĩnh; v2.0.1 chưa có DHCP.
- Không hỗ trợ RDM vì phần cứng chỉ phát, không có DE/RE riêng và đường RX DMX.
- Chỉ có cách ly galvanic nếu module RS485 thực tế hỗ trợ cách ly.
- REST API thay đổi cấu hình bằng HTTP GET để đơn giản hóa web server nhúng. Chỉ nên dùng node trong mạng lighting LAN đáng tin cậy.
- Nhiệt độ là giá trị ước tính bên trong RP2040, không phải nhiệt độ hộp được hiệu chuẩn.

---

## 5. Danh Sách Phần Cứng — Hardware BOM

| SL | Linh kiện | Yêu cầu tối thiểu | Mục đích / lưu ý |
|---:|---|---|---|
| 1 | Raspberry Pi Pico | Pico RP2040 tiêu chuẩn, flash 2 MB | Bộ xử lý chính |
| 1 | Expansion board cho Pico | Input DC 7–12 V, có rail ổn áp | Nhận nhánh nguồn 12 V và cấp nguồn hệ thống Pico |
| 1 | Module Ethernet W5500 | SPI, logic 3.3 V | Giao tiếp mạng LAN có dây |
| 4 | Module TTL-to-RS485 auto-direction | Tối thiểu 250 kbit/s, tương thích logic 3.3 V | Một module cho mỗi DMX output |
| 4 | Jack XLR-5 cái gắn panel | Loại chassis chất lượng tốt | Cổng DMX output |
| 1 | Router DC 12 V | Có cổng LAN | Kết nối node và máy điều khiển |
| 1 | Nguồn DC 12 V | Tính theo tổng router + node; thường từ 2 A sau khi tính tải | Nguồn chính |
| 1 | Bộ chia nguồn hai nhánh có cầu chì | Cầu chì riêng cho node và router | Chia nguồn 12 V an toàn |
| 1 | Dây mạng | Cat5e hoặc Cat6 | W5500 đến router |
| 1 bộ | Dây điện và terminal | Dây SPI/TTL ngắn, dây nguồn đúng tiết diện | Đấu nối bên trong |
| 1 | Hộp thiết bị | Có khe thông gió, có thể tự thiết kế hộp in 3D | Bảo vệ cơ khí |
| Tùy chọn | LED trạng thái + điện trở | Điện trở nối tiếp 220 Ω–1 kΩ | Dùng GPIO thực sự được đưa ra ngoài; xem cảnh báo GP23 |
| Tùy chọn | Tụ 100 nF | Một tụ gần nguồn mỗi module | Decoupling cao tần |
| Tùy chọn | Tụ nguồn lớn | 220–470 µF trên rail ổn áp | Hấp thụ tải biến thiên |
| Tùy chọn | Terminator DMX | 120 Ω giữa chân 2 và 3 | Dùng ở cuối dây DMX, không tự động gắn tại mọi output node |

> [!CAUTION]
> Kiểm tra đúng điện áp in trên module W5500 và RS485. Các module có hình thức giống nhau có thể sử dụng IC, regulator và dải VCC khác nhau. GPIO RP2040 dùng logic 3.3 V và không chịu được 5 V.

### Kiến trúc nguồn 12 V đề xuất

```text
Nguồn DC 12 V
      │
      ├── Cầu chì A ──> Input 7–12 V của expansion board Pico
      │                   ├── Raspberry Pi Pico
      │                   ├── Module W5500
      │                   └── 4 module RS485
      │
      └── Cầu chì B ──> Input 12 V của router
```

Chọn nguồn đủ dòng cho cả hai nhánh và giữ kết nối GND chắc chắn ở phần điện áp thấp của node.

---

## 6. Sơ Đồ Chân Kết Nối — Pinout & Wiring

<p align="center">
  <a href="[https://github.com/duongtruongdp/ND-PicoDMX-System]"><img src="images/ND DMX SYSTEM WIRING DIAGRAM.jpg" alt="ND DMX NODE 4U Wiring Diagram" width="100%"></a>
</p>

### W5500 ↔ Raspberry Pi Pico

| Tín hiệu W5500 | GPIO / nguồn Pico | Tên trong firmware | Chức năng |
|---|---|---|---|
| MISO | GP16 | `PIN_MISO` | Dữ liệu SPI từ W5500 về Pico |
| CS / SCS | GP17 | `PIN_CS` | Chip select W5500 |
| SCK | GP18 | `PIN_SCK` | Clock SPI |
| MOSI | GP19 | `PIN_MOSI` | Dữ liệu SPI từ Pico sang W5500 |
| RST | GP20 | `PIN_RST` | Reset phần cứng, active LOW |
| VCC | Rail ổn áp đúng theo module | — | Làm theo ký hiệu trên module W5500 |
| GND | GND chung | — | Mốc nguồn và logic |
| RJ45 | Cổng LAN router | — | Kết nối Ethernet |

### GPIO DMX ↔ module RS485 auto-direction

Bảng dưới đây theo đúng wiring đã được dùng trong project: GPIO output của Pico nối vào chân module ghi **TXD**, chân **RXD** bỏ trống.

| DMX output | GPIO Pico | Chân TTL module RS485 | Kết nối TTL khác | Chức năng |
|---:|---:|---|---|---|
| Port 1 | GP8 | TXD | VCC → 3.3 V, GND → GND, RXD → không nối | Output universe 1 |
| Port 2 | GP9 | TXD | VCC → 3.3 V, GND → GND, RXD → không nối | Output universe 2 |
| Port 3 | GP14 | TXD | VCC → 3.3 V, GND → GND, RXD → không nối | Output universe 3 |
| Port 4 | GP15 | TXD | VCC → 3.3 V, GND → GND, RXD → không nối | Output universe 4 |

> [!WARNING]
> Tên chân TTL trên các module RS485 giá rẻ không thống nhất. Một số module dùng `RXD`, `DI` hoặc `DIN` làm đầu vào từ TX của vi điều khiển thay vì ghi `TXD`. README này mô tả revision đã test thành công theo cách **GPIO Pico → TXD module**. Hãy kiểm tra silkscreen hoặc tài liệu đúng của module trước khi cấp nguồn.

### Module RS485 ↔ jack XLR-5

Lặp lại cho cả bốn cổng:

| Cọc RS485 | Chân XLR-5 | Tín hiệu DMX |
|---|---:|---|
| GND | 1 | Common / shield |
| B− | 2 | Data− |
| A+ | 3 | Data+ |
| — | 4 | Không dùng |
| — | 5 | Không dùng |

### Đấu nguồn

| Từ | Đến | Chức năng |
|---|---|---|
| Dương nguồn 12 V | Đầu vào bộ chia có cầu chì | Nguồn dương chính |
| Nhánh A | Input 7–12 V expansion board | Cấp nguồn node |
| Nhánh B | Input 12 V router | Cấp nguồn router |
| Âm nguồn 12 V | Âm expansion board và âm router | Đường hồi nguồn chung |
| Rail 3.3 V expansion board | VCC bốn module RS485 nếu module hỗ trợ | Nguồn transceiver/logic RS485 |
| Rail ổn áp expansion board | VCC W5500 đúng theo ký hiệu module | Nguồn module Ethernet |

### Cảnh báo chân trạng thái GP23

Firmware hiện tại định nghĩa:

```cpp
#define HARDWARE_LED_PIN 23
```

và đảo trạng thái chân này mỗi 300 ms. Trên **Raspberry Pi Pico tiêu chuẩn**, GP23 là tín hiệu nội bộ điều khiển chế độ power-save của SMPS và không được đưa ra header 40 chân. Không nên xem GP23 là chân LED ngoài.

Trước khi hoàn thiện phần cứng, chọn một trong các phương án:

1. Đổi `HARDWARE_LED_PIN` thành **GP25** để dùng LED onboard của Pico.
2. Đổi sang một GPIO trống được đưa ra ngoài và mắc LED qua điện trở hạn dòng.
3. Tắt phần heartbeat LED nếu không còn GPIO trống.

Hiệu ứng hiện tại là heartbeat đơn giản: khoảng 300 ms sáng và 300 ms tắt. Nó cho biết vòng lặp Core 0 còn chạy, không trực tiếp phản ánh packet DMX.

---

## 7. Hướng Dẫn Build & Biên Dịch

### 7.1 Phần mềm cần thiết

- Arduino IDE 2.x.
- Arduino core Raspberry Pi Pico/RP2040 của Earle F. Philhower.
- Thư viện Arduino `Ethernet` hỗ trợ W5500.
- File firmware `.ino` trong repository.

Không cần thư viện DMX ngoài. Firmware tự tạo DMX bằng RP2040 PIO và kiến trúc hai nhân.

### 7.2 Cài board package RP2040

Trong Arduino IDE:

1. Mở **Arduino IDE → Settings / Preferences**.
2. Thêm Boards Manager URL:

   ```text
   https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
   ```

3. Mở **Boards Manager**.
4. Tìm **Raspberry Pi Pico/RP2040/RP2350**.
5. Cài package của **Earle F. Philhower**.

### 7.3 Cài thư viện Ethernet

Mở **Library Manager**, tìm **Ethernet** và cài Arduino Ethernet library.

### 7.4 Cấu hình Arduino đề xuất

| Mục | Giá trị |
|---|---|
| Board | Raspberry Pi Pico |
| CPU Speed | 133 MHz |
| Flash Size | 2 MB, không cần filesystem |
| USB Stack | Pico SDK |
| Optimize | Small (`-Os`) |
| Serial Monitor | 115200 baud |

> [!IMPORTANT]
> Không chọn flash 16 MB cho Raspberry Pi Pico tiêu chuẩn 2 MB. EEPROM giả lập phụ thuộc vào layout flash đã chọn.

### 7.5 Biên dịch và upload

1. Đặt file firmware vào folder có cùng tên cơ sở với file `.ino`.
2. Nếu không thấy serial port, giữ **BOOTSEL** khi cắm Pico.
3. Chờ ổ `RPI-RP2` xuất hiện.
4. Nhấn **Upload** trong Arduino IDE.
5. Sau khi chép UF2 xong, ổ đĩa biến mất và Pico tự reboot.
6. Mở Serial Monitor ở 115200 baud để xem boot log.
7. Kết nối W5500 vào router và truy cập:

   ```text
   http://10.10.10.10/
   ```
<p align="center">
  <a href="[https://github.com/duongtruongdp/ND-PicoDMX-System]"><img src="images/WEB DASHBOARD - 01.png" alt="Live Dashboard" width="100%"></a>
</p>
<p align="center">
  <a href="[https://github.com/duongtruongdp/ND-PicoDMX-System]"><img src="images/WEB DASHBOARD - 02.png" alt="Live Dashboard" width="100%"></a>
</p>
<p align="center">
  <a href="[https://github.com/duongtruongdp/ND-PicoDMX-System]"><img src="images/WEB DASHBOARD - 03.png" alt="Live Dashboard" width="100%"></a>
</p>

### 7.6 Checklist test đầu tiên

- [ ] LED link trên W5500 sáng.
- [ ] `http://10.10.10.10/api/status` trả JSON.
- [ ] Dashboard báo `SYSTEM ONLINE`.
- [ ] Test mode `FULL` điều khiển được thiết bị DMX.
- [ ] Nguồn Art-Net/sACN hiện sender IP và FPS.
- [ ] Cả bốn cổng có DMX output khoảng 40 Hz.
- [ ] Nhiệt độ hiện giá trị hợp lý hoặc `SENSOR ERROR`, không hiện số cực đoan sai.

---

## 8. Tài Liệu REST API

HTTP server chạy TCP port **80**. Firmware hiện tại dùng các endpoint GET nhẹ để đơn giản hóa web stack nhúng.

> [!WARNING]
> Không có authentication và TLS. Chỉ sử dụng node trong mạng LAN điều khiển ánh sáng đáng tin cậy và được cách ly.

### 8.1 `GET /api/status`

Trả telemetry thiết bị, mạng, bốn cổng DMX và event log.

```bash
curl http://10.10.10.10/api/status
```

Ví dụ response:

```json
{
  "model": "ND DMX NODE 4U",
  "firmware": "2.0.1-temp-fix",
  "hardware": "RP2040 + W5500 + 4x RS485",
  "mac": "02:4E:44:34:55:01",
  "ip": "10.10.10.10",
  "link": true,
  "mode": 0,
  "start": 1,
  "merge": 0,
  "uptime": 3600,
  "packets": 144000,
  "temp": 41.7,
  "ports": [
    {
      "port": 1,
      "en": 1,
      "universe": 1,
      "active": true,
      "fps": 40.0,
      "dmxFps": 40.0,
      "used": 128,
      "age": 8,
      "test": 0,
      "proto": "Art-Net",
      "sender": "10.10.10.20"
    }
  ]
}
```

`temp` là số độ C hoặc JSON `null` khi cảm biến nội bộ trả dữ liệu không hợp lệ.

#### Các field cấp thiết bị

| Field | Ý nghĩa |
|---|---|
| `model` | Model thiết bị |
| `firmware` | Phiên bản firmware |
| `hardware` | Mô tả phần cứng |
| `build` | Ngày giờ compile |
| `mac` | Địa chỉ MAC Ethernet |
| `ip` | IPv4 hiện tại |
| `link` | Trạng thái link vật lý W5500 |
| `mode` | `0` tuần tự, `1` thủ công |
| `start` | Universe bắt đầu ở chế độ tuần tự |
| `merge` | `0` LTP, `1` HTP, `2` BLOCK |
| `uptime` | Số giây từ lúc boot |
| `c0`, `c1` | Bộ đếm loop của hai core trong chu kỳ report |
| `net` | Thời gian xử lý network lớn nhất, đơn vị µs |
| `packets` | Tổng packet Art-Net/sACN hợp lệ |
| `temp` | Nhiệt độ nội bộ RP2040 hoặc `null` |
| `ports` | Mảng trạng thái bốn cổng |
| `events` | Event log, sự kiện mới nhất nằm trước |

#### Field từng port

| Field | Ý nghĩa |
|---|---|
| `port` | Số port hiển thị 1–4 |
| `en` | Enable 0 hoặc 1 |
| `pmode` | 0 AUTO, 1 Art-Net, 2 sACN |
| `universe` | Universe đang route |
| `active` | Có packet trong vòng một giây |
| `conflict` | Phát hiện IP nguồn thứ hai |
| `fps` | FPS packet mạng được chấp nhận |
| `dmxFps` | FPS frame DMX thực tế |
| `used` | Chỉ số channel không-zero cao nhất, không phải tổng số channel khác zero |
| `age` | Thời gian từ packet cuối, đơn vị ms |
| `prio` | Priority sACN hiện tại hoặc giá trị cố định của Art-Net |
| `test` | Test mode 0–4 |
| `proto` | Tên protocol active |
| `sender` | IPv4 nguồn hiện tại |
| `c_ip` | IPv4 nguồn gây conflict |

### 8.2 `GET /api/test`

Đổi output-test mode.

| Parameter | Dải | Ý nghĩa |
|---|---:|---|
| `p` | `0`–`3` | Chỉ số port bắt đầu từ 0 |
| `m` | `0`–`4` | Test mode |

| `m` | Chế độ |
|---:|---|
| 0 | Dừng test / trở lại network |
| 1 | Blackout |
| 2 | Toàn bộ channel 50% |
| 3 | Toàn bộ channel 100% |
| 4 | Channel chase |

```bash
# Full output tại Port 1 vật lý
curl "http://10.10.10.10/api/test?p=0&m=3"

# Dừng test và trả Port 1 về network
curl "http://10.10.10.10/api/test?p=0&m=0"
```

Response thành công:

```json
{"ok":true,"message":"Test mode updated"}
```

### 8.3 `GET /api/config`

Cập nhật routing và lưu cấu hình port.

Tất cả parameter phải có mặt vì firmware validate toàn bộ bốn port trước khi lưu.

| Parameter | Giá trị | Ý nghĩa |
|---|---|---|
| `mode` | `0`, `1` | Routing tuần tự hoặc thủ công |
| `start` | `1`–`63996` | Universe bắt đầu ở chế độ tuần tự |
| `merge` | `0`, `1`, `2` | LTP, HTP hoặc BLOCK |
| `u0`…`u3` | `1`–`63999` | Universe thủ công của port 1–4 |
| `e0`…`e3` | `0`, `1` | Tắt hoặc bật port 1–4 |
| `p0`…`p3` | `0`, `1`, `2` | AUTO, chỉ Art-Net hoặc chỉ sACN |

Ví dụ Universe 1–4 tuần tự, tất cả port ON, AUTO và LTP:

```bash
curl "http://10.10.10.10/api/config?mode=0&start=1&merge=0&u0=1&e0=1&p0=0&u1=2&e1=1&p1=0&u2=3&e2=1&p2=0&u3=4&e3=1&p3=0"
```

```json
{"ok":true,"message":"Configuration saved"}
```

Lưu cấu hình sẽ ghi EEPROM và reset trạng thái nguồn mạng hiện tại của bốn port.

### 8.4 Route khác

| Route | Hành vi |
|---|---|
| `GET /` | Dashboard nội bộ |
| `GET /index.html` | Dashboard nội bộ |
| `GET /set?...` | Route cấu hình cũ; redirect về `/` khi thành công |
| `GET /favicon.ico` | HTTP 204 |
| Route khác | HTTP 404 `Not found` |

---

## 9. Cấu Hình Mạng Mặc Định

### IPv4 mặc định

| Mục | Giá trị |
|---|---|
| IP node | `10.10.10.10` |
| Subnet mask | `255.255.255.0` (`/24`) |
| Gateway | `10.10.10.1` |
| DNS | `10.10.10.1` |
| MAC | `02:4E:44:34:55:01` |
| HTTP dashboard | TCP 80 |
| Art-Net | UDP 6454 |
| sACN / E1.31 | UDP 5568 |
| Chế độ địa chỉ | IPv4 tĩnh |

Cấu hình gợi ý cho máy tính:

```text
IP address:  10.10.10.20
Subnet mask: 255.255.255.0
Gateway:     10.10.10.1
```

### Routing mặc định khi EEPROM trống hoặc không hợp lệ

| Mục | Giá trị |
|---|---|
| Routing mode | Tuần tự |
| Start universe | 1 |
| Universe Port 1 | 1 |
| Universe Port 2 | 2 |
| Universe Port 3 | 3 |
| Universe Port 4 | 4 |
| Port enable | Tất cả ON |
| Protocol filter | AUTO cho cả bốn cổng |
| Merge policy | LTP |

### Ví dụ multicast sACN

| Universe | Multicast group |
|---:|---|
| 1 | `239.255.0.1` |
| 2 | `239.255.0.2` |
| 256 | `239.255.1.0` |

---

## Tham Khảo Và Nguồn Cảm Hứng

- Tài liệu Raspberry Pi Pico: [Raspberry Pi documentation](https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html)
- Tài liệu Arduino-Pico: [arduino-pico.readthedocs.io](https://arduino-pico.readthedocs.io/)

---

<p align="center">
  <strong>ND DMX SYSTEM · Built for practical DIY lighting control.</strong><br>
  <strong>Được xây dựng cho các hệ thống điều khiển ánh sáng DIY thực tế.</strong>
</p>
