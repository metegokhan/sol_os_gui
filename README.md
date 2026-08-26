# SolarOS

SolarOS is a small ESP32 operating environment for pocket terminals,
reflective displays, serial consoles, and low-power embedded tools. It provides
a shell, foreground applications, background jobs, storage, networking,
hardware services, Python, and Lua.

## User manual

The [SolarOS User Manual](doc/manual/README.md) is the canonical source for:

- the documentation browsed on GitHub;
- the signed on-device `help` tree and `man`;
- the native agent's SolarOS reference;
- the generated documentation on solar-os.eu.

It contains the complete command, application, job, board, expansion, Python,
Lua, package, and workflow documentation. Edit the topic in `doc/manual/`;
do not maintain a separate device or website copy.

## Build

SolarOS uses PlatformIO with ESP-IDF through the pioarduino Espressif32
platform:

```sh
pio run -e waveshare_esp32_s3_rlcd_4_2
pio run -e elecrow_crowpanel_esp32_s3_4_2_epaper
pio run -e odroid_go
pio run -e esp32_s3_devkitc1_n16r8
pio run -t upload
pio device monitor -b 115200
```

The default build uses the full firmware flavor. For a smaller image:

```sh
SOLAR_OS_FLAVOR=core pio run -e waveshare_esp32_s3_rlcd_4_2
SOLAR_OS_FLAVOR=writerdeck pio run -e elecrow_crowpanel_esp32_s3_4_2_epaper
```

See [Boards and hardware targets](doc/manual/boards.md) and
[Firmware packages and flavors](doc/manual/packages.md) for the complete build
and target reference.

## Developer references

- [Service concurrency contract](doc/service-concurrency.md)
- [Memory and task-admission policy](doc/memory-policy.md)
- [OTA release schema](doc/solar_os_ota_schema.md)
- [Manual generation and signed refresh](doc/manual-system.md)

## Architecture

![Architecture diagram](doc/solar_os_architecture.png)

```text
src/apps/       foreground applications
src/jobs/       background job implementations
src/services/   shared OS services and runtime policy
src/shell/      shell command implementations
src/drivers/    low-level hardware drivers
boards/         board profiles and driver selection
include/boards/ board pin and capability metadata
packages/       package and flavor catalog
doc/manual/     canonical user manual for GitHub, device, agent, and website
doc/            developer contracts and documentation-system design
```

## Third-party software

SolarOS is licensed under the [Apache License 2.0](LICENSE.md). It also
includes the third-party software below under each project's own license.
Copyright, attribution, patent, and license notices supplied with these
components remain applicable and must be preserved in redistributions.

| Component | Used for | License and attribution |
| --- | --- | --- |
| [Lua 5.4.8](https://www.lua.org/ftp/lua-5.4.8.tar.gz) | Embedded Lua VM and selected standard libraries | MIT; copyright Lua.org, PUC-Rio. The upstream notice is retained in [`lua.h`](components/lua/lua/src/lua.h). |
| [MicroPython `d901e98349`](https://github.com/micropython/micropython/commit/d901e98349) | Embedded Python runtime | MIT; Damien P. George and MicroPython contributors. Notices are retained in the vendored source files. |
| [ESP-DSP 1.8.x](https://github.com/espressif/esp-dsp) | ESP32-S3 PIE-accelerated DSP kernels | Apache-2.0; Espressif Systems and contributors. The managed component includes the upstream `LICENSE` and notice metadata. |
| [minimp3 `ca7c706`](https://github.com/lieff/minimp3/commit/ca7c706001331a5a8e3182ce3b3ce3b243589154) | MP3 decoding | CC0-1.0. The pinned header history credits lieff, Jörn Heusipp, Alibek Omarov, Chris Robinson, Darryl T. Agostinelli, David Reid, Martin Fiedler, and Matthijs van Duin. |
| [stb_image 2.30 (`013ac3b`)](https://github.com/nothings/stb/commit/013ac3beddff3dbffafd5177e7972067cd2b5083) | PNG, JPEG, GIF, and other image decoding | MIT or public domain/Unlicense. The detailed upstream contributor and feature credits are retained in [`stb_image.h`](components/stb_image/include/stb_image.h). |
| [U8g2 `e4a5822`](https://github.com/olikraus/u8g2/commit/e4a582214cd4489307917e5decc8d3ee9597eb4a) | Monochrome graphics, text rendering, and selected display drivers | BSD-2-Clause; olikraus and contributors. See the retained [`LICENSE`](components/u8g2/LICENSE), including its separate font notices. |
| [libwebp `3757b8a`](https://github.com/webmproject/libwebp/commit/3757b8afeb54e305eaef18502812a9a88b7ed662) | WebP decoding | BSD-3-Clause; Google and contributors. See the retained [`AUTHORS`](components/webp_decoder/libwebp/AUTHORS), [`COPYING`](components/webp_decoder/libwebp/COPYING), and [`PATENTS`](components/webp_decoder/libwebp/PATENTS). |
| [MeshCore `03b6ef4`](https://github.com/meshcore-dev/MeshCore/commit/03b6ef4b0de98fc70b49ef10a6d0d61f8381fb7a) | Mesh packet, identity, contact, channel, and chat protocol subset | Separate notices are retained for MeshCore, `rweather/Crypto`, and Ed25519. See the [provenance and notice index](src/vendor/meshcore/README.solaros.md). |
| [Peanut-GB `8e65698`](https://github.com/deltabeard/Peanut-GB/commit/8e656982f08663785794b84823d3e27f856fdb7f) | Game Boy emulation and `minigb_apu` audio | MIT; Mahyar Koshkouei, Alex Baines, and contributors. See the retained [provenance and notices](src/vendor/peanut_gb/README.solaros.md). |

The SolarOS ports and local adaptations of minimp3, stb_image, U8g2, and
libwebp were integrated by nilseuropa.


# SolarOS GUI Fork

This repository is a fork of [nilseuropa's wonderful SolarOS](https://github.com/nilseuropa/solar_os), with several additional features and applications.

I added a GUI and launcher, BLE mouse and gamepad support, multitasking options, expanded keyboard connectivity, screen capture, a Wi-Fi file server, and extra applications such as a process manager, several games, BLE and Wi-Fi scanning and identification tools, a weather app, and a Pomodoro app.

Along the way, I ended up changing major system files—something I never intended when I started. It was fun... until it wasn't.

For now, this repository is a working proof of concept showing what nilseuropa's **SolarOS** is capable of and how much potential it has. I loved working with it, and many thanks to nilseuropa for the great original project.

**This fork will not receive any further updates. It works as-is. For a more stable version and future updates, please use the original [SolarOS](https://github.com/nilseuropa/solar_os) repository.**

---

## Project Overview

SolarOS is a small ESP32 operating environment for pocket terminals, reflective displays, serial consoles, and low-power embedded tools. This fork extends the original SolarOS with:

- A complete graphical user interface (GUI) with visual application launcher
- Enhanced input support including BLE mouse and gamepad
- Multitasking capabilities for concurrent application execution
- Expanded connectivity tools for Wi-Fi and BLE analysis
- Additional productivity applications and classic games

The system provides a shell, foreground applications, background jobs, storage, networking, hardware services, Python, and Lua—now with an enhanced graphical layer.

## Changes from Original SolarOS

### GUI & System
- **Graphical Launcher**: Visual application launcher with categorized folders and icons
- **Settings GUI**: Graphical system settings and control panel (`solar_os_settings_gui.c`)
- **Calendar**: Visual calendar display application (`solar_os_calendar.c`)
- **Clock**: Graphical clock application (`solar_os_clock.c`)
- **Process Manager**: Visual task, process, CPU, and memory manager (`solar_os_esprocess.c`)

### Input & Connectivity
- **BLE Mouse Support**: Bluetooth Low Energy mouse input handling
- **BLE Gamepad Support**: Game controller input via BLE
- **Keyboard Test Tool**: Live keyboard tester and input inspector (`solar_os_keyboard_test.c`)
- **Enhanced Cursor**: On-screen cursor support for mouse input

### BLE & Wi-Fi
- **BLE Scanner**: Comprehensive BLE device scanner and GATT service explorer (`solar_os_ble_scanner.c`)
- **Wi-Fi Radar**: Wi-Fi network scanner and signal analyzer (`solar_os_wifibul.c`)
- **LAN Scanner**: IP host and open port scanner for local networks (`solar_os_ag_tarayici.c`)
- **Wi-Fi Setup GUI**: Graphical Wi-Fi network connection tool (`solar_os_wifi_setup.c`)

### Multitasking
- **Background Audio**: Support for running audio applications in the background
- **Task Switching**: Enhanced application switching and management
- **Concurrent Execution**: Multiple applications can run simultaneously (with performance considerations)

### Utilities
- **File Server**: Wi-Fi-based HTTP file server for SD card access (`solar_os_file_server.c`)
- **Screen Capture**: Screenshot functionality
- **Audio dB Meter**: Sound level measurement tool (`solar_os_desibel.c`)
- **Stopwatch & Timer**: Visual timing utilities (`solar_os_stopwatch.c`, `solar_os_timer.c`)

### Applications
- **Weather App**: Graphical weather forecast display (`solar_os_weather.c`)
- **Pomodoro Timer**: Visual Pomodoro focus timer (`solar_os_pomodoro.c`)
- **Writer**: Advanced text editing application (`solar_os_writer.c`)
- **Reader**: Document viewing application (`solar_os_reader.c`)
- **View**: Image and media viewer (`solar_os_view.c`)
- **Photo Slideshow**: Photo presentation application

### Games
- **Chess**: Classic 8x8 chess game (`solar_os_chess.c`)
- **Go**: Classic 9x9 board game of Go (`solar_os_go.c`)
- **Sudoku**: 9x9 Sudoku number puzzle (`solar_os_sudoku.c`)
- **Backgammon**: Classic 24-point backgammon with AI (`solar_os_backgammon.c`)
- **Pişti**: Classic Turkish card game with AI (`solar_os_pisti.c`)
- **Blackjack**: Casino Blackjack 21 card game (`solar_os_blackjack.c`)
- **Yatzy**: 5-dice Yatzy strategy game (`solar_os_dice.c`)
- **Code Breaker**: Mastermind-style 4-peg logic challenge (integrated in launcher)
- **Invaders**: Arcade shooter game (`solar_os_invaders.c`)
- **Game Boy Emulator**: Original Game Boy emulation (`solar_os_gameboy.c`)

### Internal/System Changes
- Modified memory allocation strategies to accommodate GUI overhead
- Adjusted contiguous SRAM usage by moving allocations to PSRAM where possible
- Extended core system files to support GUI rendering and input handling
- Enhanced scheduler for improved multitasking behavior

## Included Applications

| Application | Source File | Description |
|-------------|-------------|-------------|
| Launcher | `solar_os_launcher.c` | Graphical application launcher with categorized folders |
| Weather | `solar_os_weather.c` | Graphical weather forecast display |
| Pomodoro | `solar_os_pomodoro.c` | Visual Pomodoro focus timer |
| Wi-Fi Setup | `solar_os_wifi_setup.c` | Graphical Wi-Fi network connection tool |
| Settings GUI | `solar_os_settings_gui.c` | System settings and control panel |
| File Server | `solar_os_file_server.c` | Wi-Fi SD card HTTP file server |
| Process Manager | `solar_os_esprocess.c` | Task, process, CPU and memory manager |
| Keyboard Test | `solar_os_keyboard_test.c` | Live keyboard tester and input inspector |
| BLE Scanner | `solar_os_ble_scanner.c` | BLE scanner and GATT service explorer |
| Wi-Fi Radar | `solar_os_wifibul.c` | Wi-Fi network scanner and signal analyzer |
| LAN Scanner | `solar_os_ag_tarayici.c` | LAN IP host and open port scanner |
| Calendar | `solar_os_calendar.c` | Visual calendar display |
| Clock | `solar_os_clock.c` | Graphical clock application |
| dB Meter | `solar_os_desibel.c` | Audio sound level measurement |
| Stopwatch | `solar_os_stopwatch.c` | Visual stopwatch utility |
| Timer | `solar_os_timer.c` | Visual countdown timer |
| Writer | `solar_os_writer.c` | Advanced text editor |
| Reader | `solar_os_reader.c` | Document viewer |
| View | `solar_os_view.c` | Image and media viewer |

## Games

| Game | Source File | Description |
|------|-------------|-------------|
| Chess | `solar_os_chess.c` | Classic 8x8 chess game |
| Go | `solar_os_go.c` | Classic 9x9 board game of Go |
| Sudoku | `solar_os_sudoku.c` | 9x9 Sudoku number puzzle |
| Backgammon | `solar_os_backgammon.c` | Classic 24-point backgammon with AI |
| Pişti | `solar_os_pisti.c` | Classic Turkish card game with AI |
| Blackjack | `solar_os_blackjack.c` | Casino Blackjack 21 card game |
| Yatzy | `solar_os_dice.c` | 5-dice Yatzy strategy game |
| Code Breaker | (launcher integration) | Mastermind 4-peg logic challenge |
| Invaders | `solar_os_invaders.c` | Arcade shooter game |
| Dice | `solar_os_dice.c` | Dice rolling utility |
| Game Boy | `solar_os_gameboy.c` | Original Game Boy emulator |

## Connectivity & Input Support

| Feature | Status | Description |
|---------|--------|-------------|
| Wi-Fi | ✓ | Full networking stack with file server capabilities |
| BLE Scanning | ✓ | Device discovery and GATT service exploration |
| BLE Mouse | ✓ | Bluetooth mouse cursor and click support |
| BLE Gamepad | ✓ | Game controller input handling |
| Keyboard | ✓ | Enhanced connectivity and real-time testing |
| Mouse Cursor | ✓ | On-screen cursor rendering |

## Installation / Building / Usage

SolarOS uses PlatformIO with ESP-IDF through the pioarduino Espressif32 platform.

### Build Commands

```sh
# Standard build for supported boards
pio run -e waveshare_esp32_s3_rlcd_4_2
pio run -e elecrow_crowpanel_esp32_s3_4_2_epaper
pio run -e odroid_go
pio run -e esp32_s3_devkitc1_n16r8

# Upload firmware
pio run -t upload

# Serial monitor
pio device monitor -b 115200
```

### Firmware Flavors

The default build uses the full firmware flavor. For a smaller image:

```sh
# Core flavor (minimal)
SOLAR_OS_FLAVOR=core pio run -e waveshare_esp32_s3_rlcd_4_2

# WriterDeck flavor
SOLAR_OS_FLAVOR=writerdeck pio run -e elecrow_crowpanel_esp32_s3_4_2_epaper
```

### Partition Files

Different partition schemes are available depending on your flash size:

- `partitions.csv` — Default partition layout
- `partitions_4mb.csv` — For 4MB flash devices
- `partitions_8mb.csv` — For 8MB flash devices
- `partitions_16mb_devkit.csv` — For 16MB devkit boards

See [Boards and hardware targets](doc/manual/boards.md) and [Firmware packages and flavors](doc/manual/packages.md) for complete build and target reference.

## Hardware Requirements

Based on the enhanced GUI and multitasking features, this fork requires:

- **Microcontroller**: ESP32-S3 or compatible with sufficient RAM
- **Flash**: Minimum 8MB recommended (16MB preferred for full feature set)
- **PSRAM**: External PSRAM strongly recommended for GUI operations
- **Display**: LCD or e-paper display supported by SolarOS board profiles
- **Input** (optional): BLE keyboard, mouse, or gamepad for enhanced interaction

Refer to [Boards and hardware targets](doc/manual/boards.md) for specific board configurations.

## Known Limitations / Technical Notes

### Memory Constraints
One of the main technical limitations was the available contiguous SRAM. In several cases, this was handled by reducing application memory usage or moving suitable allocations to PSRAM. The GUI layer and multitasking features increase memory pressure compared to the original SolarOS.

### Multitasking Behavior
Multitasking works for audio and basic applications. Background apps cannot automatically switch themselves to the foreground when a scheduled task is triggered, but they can be restored manually via the Task Manager (Process Manager) or by reopening them.

For example, you can listen to web radio in the background while writing text or playing a game. However, performance is limited when Wi-Fi, BLE, mouse input, the on-screen cursor, and multiple tasks are running simultaneously.

### Performance Considerations
- Running multiple wireless interfaces (Wi-Fi + BLE) concurrently may impact responsiveness
- Heavy GUI rendering combined with network activity can cause frame rate drops
- Game Boy emulation and other intensive applications perform best when run standalone
- Screen capture functionality may briefly interrupt other operations

### Experimental Features
This fork contains experimental modifications to core system files. Some features may exhibit unstable behavior under certain conditions. The changes serve as a proof of concept for extending SolarOS capabilities.

## Project Status

**This fork is no longer under active development.** It serves as a working proof of concept demonstrating:
- What the original SolarOS architecture is capable of
- How GUI layers can be integrated into the system
- The potential for multitasking on ESP32 platforms
- Various application patterns and implementations

For ongoing support, stability improvements, and future updates, please use the original [SolarOS](https://github.com/nilseuropa/solar_os) repository.

## Credits

- **[nilseuropa](https://github.com/nilseuropa)**: Creator of the original SolarOS project
- **SolarOS Contributors**: Various contributors to the SolarOS ecosystem
- **Fork Author**: Additional GUI, multitasking, applications, and games added to extend the original system

## Third-Party Software

SolarOS is licensed under the [Apache License 2.0](LICENSE.md). It also includes third-party software under their respective licenses. See the original SolarOS documentation for complete third-party attributions including:

- Lua 5.4.8 (MIT)
- MicroPython (MIT)
- ESP-DSP (Apache-2.0)
- minimp3 (CC0-1.0)
- stb_image (MIT / Unlicense)
- U8g2 (BSD-2-Clause)
- libwebp (BSD-3-Clause)
- MeshCore (various notices)
- Peanut-GB (MIT)

---

## User Manual

The [SolarOS User Manual](doc/manual/README.md) is the canonical source for:

- The documentation browsed on GitHub
- The signed on-device `help` tree and `man`
- The native agent's SolarOS reference
- The generated documentation on solar-os.eu

It contains the complete command, application, job, board, expansion, Python, Lua, package, and workflow documentation. Edit the topic in `doc/manual/`; do not maintain a separate device or website copy.

## Developer References

- [Service concurrency contract](doc/service-concurrency.md)
- [Memory and task-admission policy](doc/memory-policy.md)
- [OTA release schema](doc/solar_os_ota_schema.md)
- [Manual generation and signed refresh](doc/manual-system.md)

## Architecture

See the original SolarOS repository for the architecture diagram and detailed component documentation.

```text
src/apps/       foreground applications (including GUI additions)
src/jobs/       background job implementations
src/services/   shared OS services and runtime policy
src/shell/      shell command implementations
src/drivers/    low-level hardware drivers
boards/         board profiles and driver selection
include/boards/ board pin and capability metadata
packages/       package and flavor catalog
doc/manual/     canonical user manual
doc/            developer contracts and documentation-system design
```
