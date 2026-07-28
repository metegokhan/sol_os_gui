+++
id = "overview"
title = "SolarOS manual"
section = "concept"
summary = "Find commands, applications, jobs, scripting APIs, and hardware concepts"
aliases = ["solaros", "manual"]
keywords = "solaros api modules python lua help reference capabilities commands apps jobs"
packages_any = []
+++
# SolarOS manual

The SolarOS manual explains how to use the system that is actually installed on
your device. Optional topics appear only when their package is part of the
firmware.

Use `man --list` to browse topics or search by task:

```text
man -k draw a circle
man -k connect wifi
man -k background job memory
```

Open a result with `man TOPIC`. The arrow and Page Up/Page Down keys scroll; `/`
searches inside a page and `Ctrl+]` returns to the shell.

## Where to begin

- Use `man scripting` before writing a Python or Lua application.
- Use `man python.gfx` or `man lua.gfx` for a graphical application.
- Use `man jobs` to understand background workers and their memory.
- Use `man buses` and `man expansion` before connecting external hardware.
- Use `man identity` to configure the device user and hostname.
- Use `man docs` to refresh the signed manual on a supported device.

## Quick reference

Search again with a module or task name. Topics cover gfx, tui, storage,
identity, jobs, sessions, apps, wifi, mqtt, net, gpio, adc, pwm, buses, i2c,
spi, uart, onewire, expansion, audio, ble, clipboard, time, battery, sensors,
and ssh_keys. Optional modules exist only when their package is installed.
