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
