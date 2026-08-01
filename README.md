💡 ND DMX NODE 4U (v2.0.1)
======================

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

- Pin 1 (GND): Signal Ground / Cable Shield.
- Pin 2 (Data -): Inverted Differential Signal.
- Pin 3 (Data +): Non-inverted Differential Signal.

C. DMX512 Microsecond Timing Structure

DMX512 transmits data at a fixed baud rate of 250 kbit/s (each bit lasts exactly $4\ \mu\text{s}$). A full DMX frame consists of:
- BREAK: Signal pulled LOW for $88\ \mu\text{s} - 105\ \mu\text{s}$ to signal the start of a new frame.
- MAB (Mark After Break): Signal pulled HIGH for $12\ \mu\text{s} - 14\ \mu\text{s}$ to separate BREAK from data bytes.
- Start Code: First transmitted byte (0x00 for standard lighting data).
- 512 Data Bytes: Transmitted using UART 8N2 format (8 data bits, No parity, 2 stop bits).
