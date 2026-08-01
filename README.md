💡 ND DMX NODE 4U (v2.0.1)
======================

📌 🇻🇳 TIẾNG VIỆT

1. Tổng Quan (Overview)

ND DMX NODE 4U là thiết bị chuyển đổi giao thức điều khiển ánh sáng mạng chuyên nghiệp (Art-Net 4 & sACN ANSI E1.31 to 4-Port DMX512 Gateway) có độ trễ cực thấp. Thiết bị được vận hành bởi vi điều khiển dual-core Raspberry Pi Pico (RP2040) kết hợp với chip Ethernet phần cứng W5500.

Firmware v2.0.1 áp dụng kiến trúc Phân chia nhân độc lập (Asymmetric Multiprocessing - AMP) kết hợp khối Programmable I/O (PIO) phần cứng để phát 4 cổng DMX512 độc lập với chu kỳ thời gian thực tuyệt đối (~40 Hz)—hoàn toàn cách ly với các tác vụ nhận gói tin mạng và giao diện Web Dashboard.

2. Bối Cảnh & Lý Thuyết DMX512 (DMX Fundamentals)

A. DMX512 Là Gì?

DMX512 (Digital Multiplex 512) là chuẩn giao tiếp số chuẩn công nghiệp được sử dụng để điều khiển các thiết bị chiếu sáng sân khấu (đèn Moving Head, LED Par, máy khói, laser,...).

Universe (Vũ trụ dữ liệu): Mỗi tuyến DMX512 truyền một tập hợp gồm 512 kênh (Channels) dữ liệu độc lập.

Giá trị kênh (0 – 255): Mỗi kênh mang một giá trị số 8-bit từ 0 (Tắt / 0%) đến 255 (Sáng tối đa / 100%).

Địa chỉ gán (DMX Address): Thiết bị đèn sẽ đọc chuỗi kênh liên tiếp tính từ địa chỉ bắt đầu được cài đặt.

B. Chuẩn Truyền Tín Hiệu Vật Lý RS-485

DMX512 truyền tín hiệu vi sai qua chuẩn RS-485 trên dây cáp xoắn đôi chống nhiễu (kết nối XLR 3-pin hoặc 5-pin):

Pin 1 (GND): Dây mát / Bọc kim chống nhiễu.

Pin 2 (Data -): Tín hiệu vi sai âm.

Pin 3 (Data +): Tín hiệu vi sai dương.

C. Cấu Trúc Khung Tín Hiệu DMX512 (Microsecond Timing)

Tín hiệu DMX512 phát dữ liệu theo tốc độ bit cố định 250 kbit/s (mỗi bit kéo dài đúng $4\ \mu\text{s}$). Mỗi khung dữ liệu DMX (Frame) bao gồm:

 HIGH  ──┐                                  ┌───┐   ┌───┐       ┌───
         │                                  │   │   │   │       │
 LOW     └─── BREAK ──────────►┌─ MAB ──┐   └───┴───┘   └─── ...┴───
         │◄── 88µs - 105µs ──►│◄12-14µs►│   StartCode   Data Byte 1..512


BREAK (Xung ngắt): Tín hiệu bị kéo xuống mức LOW trong khoảng $88\ \mu\text{s} - 105\ \mu\text{s}$ để báo hiệu bắt đầu một khung DMX mới.

MAB (Mark After Break): Mức HIGH kéo dài $12\ \mu\text{s} - 14\ \mu\text{s}$ để phân tách xung BREAK với dữ liệu.

Start Code: Byte định danh đầu tiên (0x00 đối với dữ liệu đèn tiêu chuẩn).

512 Data Bytes: 512 byte dữ liệu truyền theo định dạng UART 8N2 (8 bit dữ liệu, No parity, 2 stop bits).

D. DMX Qua Mạng Ethernet (Art-Net & sACN)

Art-Net 4 (UDP Port 6454): Giao thức đóng gói DMX qua UDP, hỗ trợ cơ chế tự động dò tìm thiết bị (ArtPoll / ArtPollReply).

sACN / ANSI E1.31 (UDP Port 5568): Giao thức chuẩn hóa quốc tế, hỗ trợ truyền Unicast và Multicast (239.255.A.B) cùng trường độ ưu tiên (Priority 0–200).

3. Cách Thức DMX Hoạt Động Trong Mã Nguồn Code (Firmware Internals)

Firmware v2.0.1 giải quyết triệt để sự cố đơ lag hoặc sụt xung DMX bằng các kỹ thuật lập trình nhúng cấp cao trên RP2040:

A. Kiến Trúc Dual-Core Phân Chia Nhiệm Vụ (AMP Architecture)

Core 0 (setup() / loop()) - Mạng & Web:

Lắng nghe và bóc tách gói UDP Art-Net / sACN.

Phản hồi các gói phát hiện ArtPollReply.

Vận hành REST API Server và Web Dashboard.

Ghi dữ liệu mạng nhận được vào bộ đệm ẩn (Back-Buffer dmxBuffer).

Lọc nhiễu cảm biến nhiệt độ ADC4 nội bộ của RP2040.

Core 1 (setup1() / loop1()) - Engine DMX Thời Gian Thực:

Cách ly hoàn toàn khỏi các tác vụ Ethernet.

Vận hành bộ máy trạng thái (State Machine) phát tín hiệu DMX với tần số chuẩn 40 Hz (mỗi frame $25\text{ ms}$).

Đọc dữ liệu từ bộ đệm hiển thị (Front-Buffer activeDmxBuffer).

B. Khối PIO (Programmable I/O) DMX Driver

Thay vì dùng phần mềm ngắt CPU (Software Bit-Banging) hay UART truyền thống không tạo được xung BREAK ngắn:

Lập trình State Machine của khối PIO0 và PIO1 trên RP2040 chạy ở tần số clock $1\text{ MHz}$ ($1\text{ cycle} = 1\ \mu\text{s}$).

PIO tự động rút từng byte từ TX FIFO để phát tín hiệu UART $250\text{ kbit/s}$ định dạng 8N2 mà không tốn % CPU nào.

// Lệnh PIO ASM đóng gói phát DMX 8N2 chuẩn 250 kbit/s
static const uint16_t dmx_program_instructions[] = {
    0x98a0, // pull block side 1
    0xf327, // set x, 7 side 0 [3]
    0x6201, // out pins, 1 [2]
    0x0042, // jmp x--, 2
    0xbe42  // nop side 1 [6]
};


C. Cơ Chế Double-Buffering & Hardware Spinlock Protection

Để tránh tình trạng Core 0 đang ghi dữ liệu mạng dở dang thì Core 1 lại đọc xuất ra DMX gây ra hiện tượng rác dữ liệu (Tearing):

Mỗi port sở hữu 2 bộ đệm độc lập: dmxBuffer (Back-buffer cho Core 0) và activeDmxBuffer (Front-buffer cho Core 1).

Việc hoán đổi dữ liệu giữa 2 bộ đệm được bảo vệ bởi con khóa Hardware Spinlock (dmx_lock) của RP2040.

[ Gói tin Art-Net / sACN ] 
           │
           ▼ (Core 0 nhận)
  ┌─────────────────┐       Hardware Spinlock        ┌──────────────────────┐
  │   dmxBuffer     │ ─────────────────────────────► │  activeDmxBuffer     │ ──► [ Khối PIO ] ──► RS485 Transceiver ──► DMX Out
  │  (Back-Buffer)  │   (Atomic Swap khi có data)    │    (Front-Buffer)    │       (Core 1 phát 40Hz)
  └─────────────────┘                                └──────────────────────┘


4. Tính Năng Nổi Bật (Key Features)

🚀 Task Isolation Đa Nhân: Cách ly hoàn toàn tác vụ Mạng (Core 0) và tác vụ Xuất tín hiệu DMX (Core 1).

⚡ Tần Số Đáp Ứng Cố Định 40Hz: Đảm bảo xuất khung DMX liên tục $25\text{ ms}$ không bị tụt xung ngay cả khi dữ liệu đứng yên.

🌐 Hỗ Trợ Đa Giao Thức Mạng:

Art-Net 4 (UDP 6454): Hỗ trợ ArtPoll & ArtPollReply tự động dò tìm Node.

sACN ANSI E1.31 (UDP 5568): Hỗ trợ Unicast & Multicast (239.255.A.B) cho 4 Universe với quản lý độ ưu tiên (Priority).

🔀 Smart Signal Merging:

LTP (Latest Takes Precedence): Đồ họa/nguồn gửi tới sau cùng sẽ ghi đè.

HTP (Highest Takes Precedence): Trộn lấy giá trị kênh lớn nhất từ 2 nguồn.

BLOCK: Khóa cứng nguồn thứ 2 khi nguồn thứ 1 đang phát.

⚠️ Source Conflict Detection: Phát hiện & cảnh báo trùng lặp địa chỉ IP nguồn gửi trùng Universe.

🖥️ Zero-Dependency Embedded Web Dashboard:

Giao diện dark-mode Cyber-Industrial tích hợp sẵn trong bộ nhớ Flash.

Giám sát thời gian thực: FPS ngõ vào/ra, nhiệt độ vi điều khiển RP2040 (xử lý ổn định loại bỏ nhiễu ở v2.0.1), kênh hoạt động, đếm vòng lặp từng nhân.

🛠️ Integrated Test Mode: Kiểm tra phần cứng trực tiếp từ Web Dashboard (BLACKOUT, 50%, FULL, CHASE).

5. Sơ Đồ Chân Kết Nối (Pinout & Wiring)

W5500 Ethernet Module (SPI0 Interface)

W5500 Pin

RP2040 Pin

Chức Năng

MISO

GPIO 16

SPI0 RX

CS / SS

GPIO 17

Chip Select

SCK / SCLK

GPIO 18

SPI0 Clock

MOSI

GPIO 19

SPI0 TX

RST

GPIO 20

Hardware Reset W5500

VCC

3.3V

Nguồn module

GND

GND

Dây mát

DMX Outputs (RS485 Transceivers)

Cổng DMX

Chân TX RP2040

Chân Module RS485

Sơ Đồ Cổng XLR

Port 1

GPIO 8

DI (Data In)

Pin 1: GND | Pin 2: Data- | Pin 3: Data+

Port 2

GPIO 9

DI (Data In)

Pin 1: GND | Pin 2: Data- | Pin 3: Data+

Port 3

GPIO 14

DI (Data In)

Pin 1: GND | Pin 2: Data- | Pin 3: Data+

Port 4

GPIO 15

DI (Data In)

Pin 1: GND | Pin 2: Data- | Pin 3: Data+

Lưu ý: Nối chân DE (Data Enable) và /RE (Receiver Enable) của cả 4 chip RS485 lên mức cao VCC (3.3V) để cố định chip ở chế độ Chỉ Phát (TX-Only).

6. Cấu Hình Mạng Mặc Định (Network Defaults)

Static IP Node: 10.10.10.10

Subnet Mask: 255.255.255.0

Gateway / DNS: 10.10.10.1

Dashboard Web: http://10.10.10.10/

Art-Net UDP Port: 6454

sACN UDP Port: 5568

📌 🇬🇧 ENGLISH

1. Overview

ND DMX NODE 4U is an ultra-low latency professional lighting control network gateway (Art-Net 4 & sACN ANSI E1.31 to 4-Port DMX512 Gateway). The device is powered by the dual-core Raspberry Pi Pico (RP2040) microcontroller combined with a hardware Ethernet TCP/IP controller W5500.

Firmware v2.0.1 utilizes an Asymmetric Multiprocessing (AMP) architecture combined with hardware Programmable I/O (PIO) blocks to output 4 independent DMX512 ports at a deterministic real-time frame rate (~40 Hz)—completely isolated from network packet handling and web dashboard tasks.

2. DMX Fundamentals

A. What is DMX512?

DMX512 (Digital Multiplex 512) is the industry standard digital communication protocol used for controlling stage lighting equipment (Moving Heads, LED Pars, Fog Machines, Lasers, etc.).

Universe: Each DMX512 line transmits a collection of 512 independent Channels.

Channel Value (0 – 255): Each channel carries an 8-bit numerical value ranging from 0 (Off / 0%) to 255 (Full / 100%).

DMX Address: Lighting fixtures read a block of sequential channels starting from their assigned base address.

B. RS-485 Physical Layer

DMX512 transmits differential signals over RS-485 via shielded twisted-pair cables (3-pin or 5-pin XLR):

Pin 1 (GND): Signal Ground / Cable Shield.

Pin 2 (Data -): Inverted Differential Signal.

Pin 3 (Data +): Non-inverted Differential Signal.

C. DMX512 Microsecond Timing Structure

DMX512 transmits data at a fixed baud rate of 250 kbit/s (each bit lasts exactly $4\ \mu\text{s}$). A full DMX frame consists of:

BREAK: Signal pulled LOW for $88\ \mu\text{s} - 105\ \mu\text{s}$ to signal the start of a new frame.

MAB (Mark After Break): Signal pulled HIGH for $12\ \mu\text{s} - 14\ \mu\text{s}$ to separate BREAK from data bytes.

Start Code: First transmitted byte (0x00 for standard lighting data).

512 Data Bytes: Transmitted using UART 8N2 format (8 data bits, No parity, 2 stop bits).

D. DMX Over Ethernet Protocols

Art-Net 4 (UDP Port 6454): DMX encapsulated over UDP with automatic node discovery (ArtPoll / ArtPollReply).

sACN / ANSI E1.31 (UDP Port 5568): International standard protocol supporting Unicast and Multicast (239.255.A.B) with per-source Priority management (0–200).

3. Firmware Internals & Code Architecture

Firmware v2.0.1 eliminates frame drops and latency spikes using low-level embedded programming techniques on RP2040:

A. Dual-Core Task Isolation (AMP Architecture)

Core 0 (setup() / loop()) - Network & Web:

Parses incoming UDP Art-Net / sACN packets.

Responds to ArtPollReply node discovery requests.

Hosts REST API and Web Dashboard.

Writes incoming universe data to the Back-Buffer (dmxBuffer).

Filters RP2040 internal ADC4 temperature sensor noise.

Core 1 (setup1() / loop1()) - Deterministic DMX Engine:

Completely isolated from Ethernet operations.

Runs a state machine generating DMX output at a strict 40 Hz rate ($25\text{ ms}$ period).

Reads display data from the Front-Buffer (activeDmxBuffer).

B. PIO (Programmable I/O) DMX Driver

Rather than relying on CPU bit-banging or standard hardware UARTs (which cannot handle precise custom BREAK timing):

State Machines on PIO0 and PIO1 run at $1\text{ MHz}$ ($1\text{ cycle} = 1\ \mu\text{s}$).

PIO automatically shifts bytes out from TX FIFO to generate 8N2 UART signals at $250\text{ kbit/s}$ with 0% CPU overhead.

C. Double-Buffering & Hardware Spinlock Protection

To prevent tearing artifacts when Core 0 writes network data while Core 1 reads for DMX output:

Each port features two independent buffers: dmxBuffer (Back-buffer for Core 0) and activeDmxBuffer (Front-buffer for Core 1).

Buffer swaps are protected by RP2040 Hardware Spinlocks (dmx_lock).

4. Key Features

🚀 Dual-Core Task Isolation: Complete separation of Network processing (Core 0) and DMX Frame Engine (Core 1).

⚡ Deterministic 40Hz Refresh Rate: Constant $25\text{ ms}$ frame output prevents fixture flickering even when data is static.

🌐 Multi-Protocol Support:

Art-Net 4 (UDP 6454): Supports ArtPoll & ArtPollReply auto-discovery.

sACN ANSI E1.31 (UDP 5568): Unicast & Multicast (239.255.A.B) support across 4 universes with priority handling.

🔀 Smart Signal Merging:

LTP (Latest Takes Precedence): Newest packet overrides.

HTP (Highest Takes Precedence): Blends highest channel values.

BLOCK: Locks port to primary source IP.

⚠️ Source Conflict Detection: Detects and logs duplicate IP sources sending to the same Universe.

🖥️ Zero-Dependency Embedded Web Dashboard:

Dark-mode Cyber-Industrial Web UI stored in Flash memory.

Real-time telemetry: Input/Output FPS, MCU temperature (stabilized in v2.0.1), active channels, core loop counts.

🛠️ Integrated Test Mode: Hardware testing directly from Web UI (BLACKOUT, 50%, FULL, CHASE).

5. Hardware BOM

Component

Qty

Specification / Description

Microcontroller

1

Raspberry Pi Pico (RP2040 Dual ARM Cortex-M0+ @ 133MHz)

Ethernet Module

1

W5500 SPI Ethernet Module

RS485 Transceivers

4

MAX485 / SP485E / ADM485 or isolated MAX14870

Status LED

1

System Heartbeat LED (GPIO 23)

Power Supply

1

DC 5V / 2A

DMX Connectors

4

XLR 3-Pin or 5-Pin Chassis Mount

6. Build & Compilation Guide

Install Arduino IDE 2.x.

Add Board Manager URL under Preferences -> Additional Boards Manager URLs:
https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json

Install Raspberry Pi Pico/RP2040 by Earle F. Philhower, III.

Select the following settings under Tools:

Board: Raspberry Pi Pico

CPU Speed: 133 MHz

Flash Size: 2MB (no filesystem required)

USB Stack: Pico SDK

Optimize: Small (-Os)

Open ND_DMX_NODE_4U_v2_0_1.ino and click Upload.

7. REST API Documentation

Endpoint

Method

Parameters

Description

/api/status

GET

None

Returns JSON telemetry, MCU temp, port status, IP conflicts, and event logs.

/api/test

GET

p (Port: 0-3)



m (Mode: 0-4)

Overrides output mode:



0: Network, 1: Blackout, 2: 50%, 3: Full, 4: Chase.

/api/config

GET

mode, start, merge, u0-u3, e0-e3, p0-p3

Configures universe mapping, enables ports, sets protocol filters (0: Auto, 1: Art-Net, 2: sACN), and merge policies.

📄 License

Distributed under the MIT License. Free for modification, commercial use, and open-source distribution.
