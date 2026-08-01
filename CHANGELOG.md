# Changelog

All notable changes to this project will be documented in this file.

This project follows the principles of **Keep a Changelog** and uses **Semantic Versioning (SemVer)**.

---

## [2.0.1] - 2026-08-01

### ✨ Added

- Redesigned industrial-style Web Dashboard.
- Added real-time MCU temperature monitoring.
- Added DMX Output FPS monitoring.
- Added Input Packet FPS monitoring.
- Added Last Packet Age indicator.
- Added Source Quality status.
- Added Event Log panel.
- Added Port Output Test Mode:
  - Stop
  - Blackout
  - 50%
  - Full
  - Chase
- Added Channel Activity visualization.
- Added Device Information section.
- Added Firmware Version display.
- Added REST API improvements for diagnostics.

### 🚀 Improved

- Improved Web Dashboard layout with industrial UI.
- Changed typography:
  - Headline → Impact
  - Body → Helvetica Neue
  - Technical Data → Consolas
- Improved JSON API structure.
- Improved temperature filtering.
- Improved DMX statistics update.
- Improved responsiveness for desktop and tablet browsers.
- Improved system diagnostics.

### 🛠 Fixed

- Fixed MCU temperature reading showing invalid values.
- Fixed JSON output when temperature sensor returns invalid data.
- Fixed Dashboard rendering issues.
- Fixed HTML generation reliability.
- Fixed packet statistics synchronization.
- Fixed Web API stability.

---

## [2.0.0] - 2026-05-30

### 🎉 Initial Public Release

First public release of **ND DMX NODE 4U**.

### ✨ Features

- Raspberry Pi Pico based Art-Net / sACN to DMX Node.
- 4 Independent DMX512 Outputs.
- W5500 Ethernet Interface.
- Art-Net Protocol Support.
- sACN (E1.31) Protocol Support.
- Dual-Core Processing.
- PIO-based DMX512 Engine.
- Double Buffer DMX Architecture.
- Real-time Universe Mapping.
- Web Configuration Interface.
- Static IP Configuration.
- EEPROM Configuration Storage.
- Watchdog Protection.
- Network Diagnostics.
- Merge Mode Support.
- Automatic Source Detection.

### ⚡ Performance

- DMX Refresh Rate ~40 FPS.
- 512 Channels per Universe.
- Low-latency Ethernet Processing.
- Independent DMX Output Engine running on Core 1.

### 🔧 Hardware

- Raspberry Pi Pico
- W5500 Ethernet Module
- 4× Auto Direction RS485 Modules
- 4× DMX512 5-Pin XLR Outputs
- 7–12V DC Input

---

## Future Roadmap

### Planned for v2.1

- RDM Preparation
- DHCP Support
- Configuration Backup / Restore
- Password Protection
- User Login
- Dark / Light Theme
- SNMP Monitoring
- NTP Synchronization
- Improved Event Logger
- Hardware Monitoring
