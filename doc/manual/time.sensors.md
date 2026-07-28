+++
id = "time.sensors"
title = "Time, battery, and environment APIs"
section = "api"
summary = "Read clocks, battery state, temperature, and humidity"
aliases = ["time", "battery", "sensors"]
keywords = "python lua time clock date timezone ntp uptime battery sensor temperature humidity environment"
packages_any = []
+++
# Time, battery, and environment APIs

SolarOS distinguishes uptime, UTC, and configured local time. Sensor and
battery values exist only when the board and firmware provide their services.

## Check time integrity

```python
import solaros

print(solaros.time.uptime())
if solaros.time.is_valid():
    print(solaros.time.datetime())
```

Do not label data with wall-clock timestamps until the RTC or NTP-derived time
is valid. Use uptime for monotonic intervals.

## Read installed sensors

Inspect `solaros.battery.status()` and `solaros.sensors.environment()` rather
than assuming a fixed voltage, temperature, or humidity source.

## Quick reference

solaros.time provides uptime_ms, uptime, datetime, utc_datetime, set_datetime,
set_utc_datetime, utc_to_local, local_to_utc, is_valid, timezone,
set_timezone, and ntp_sync. solaros.battery.status and
solaros.sensors.environment are package-gated.
