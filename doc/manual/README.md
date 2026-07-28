# SolarOS User Manual

This is the canonical documentation used by GitHub, the generated solar-os.eu website, the signed on-device `docs` browser, `man`, and the native agent reference tool.

## Getting started

- [Browsing and refreshing documentation](docs.md) — Browse the manual and refresh its signed Markdown pages
- [SolarOS manual](overview.md) — Find commands, applications, jobs, scripting APIs, and hardware concepts
- [SolarOS scripting conventions](script.conventions.md) — Write cooperative Python and Lua programs against SolarOS services

## Shell and storage

- [Shell command reference](commands.md) — Complete syntax, behavior, and examples for built-in shell commands
- [Storage and shell paths](storage.md) — Use SolarOS volumes, files, directories, and shell-style paths

## Applications

- [Agent service and tool reference](agent.service.md) — Provider contract, typed tools, policy, resource bounds, and roadmap
- [Application reference](apps.md) — Usage, controls, and examples for every foreground application
- [Native SolarOS agent](agent.md) — Configure and use the resumable LLM agent and its typed tools

## Background jobs

- [Background job reference](jobs.reference.md) — Configuration, ownership, and examples for every background job
- [Background jobs](jobs.md) — Inspect and control bounded background workers

## Networking and security

- [SSH identity keys](ssh_keys.md) — Inspect, generate, and remove the default SSH key pair
- [Wi-Fi, MQTT, and network APIs](network.md) — Connect, inspect, and communicate over installed network services

## Hardware and expansion

- [Audio, BLE keyboard, and clipboard APIs](media.input.md) — Use installed media and input services
- [Expansion drivers and attached devices](expansion.md) — Discover, attach, and detach package-gated expansion devices
- [Expansion hardware reference](expansion.reference.md) — Resource rules, workflows, drivers, bindings, and wiring examples
- [GPIO, ADC, PWM, and LED APIs](gpio.analog.md) — Use runtime-safe digital and analog expansion pins
- [Time, battery, and environment APIs](time.sensors.md) — Read clocks, battery state, temperature, and humidity

## Scripting APIs

- [Compatibility I/O modules](compatibility.io.md) — Use the legacy single-bus I2C, SPI, UART, and OneWire APIs
- [Lua API reference](lua.md) — Complete Lua service API, conventions, and examples
- [Lua graphics API](lua.gfx.md) — Draw through SolarOS displays from Lua
- [Lua text user-interface API](lua.tui.md) — Build terminal applications from Lua
- [Named runtime buses](buses.md) — Create and use resource-owned I2C, SPI, UART, and OneWire buses
- [Python API reference](python.md) — Complete MicroPython service API, conventions, and examples
- [Python graphics API](python.gfx.md) — Draw through SolarOS displays from MicroPython
- [Python text user-interface API](python.tui.md) — Build terminal applications from MicroPython

## System services

- [Device identity](identity.md) — Read and configure the NVS-backed user and hostname
- [Foreground sessions and applications](sessions.apps.md) — Create shells and inspect resumable foreground applications

## Boards and firmware

- [Boards and hardware targets](boards.md) — Supported boards, capabilities, porting structure, and validation
- [Firmware packages and flavors](packages.md) — Understand package ownership, groups, flavors, and custom builds

The TOML frontmatter on each topic controls package availability, search metadata, and placement in the documentation tree. Edit the topic itself; do not maintain a separate device or website copy.
