+++
id = "expansion"
title = "Expansion drivers and attached devices"
section = "service"
summary = "Discover, attach, and detach package-gated expansion devices"
aliases = ["devices", "drivers"]
keywords = "python lua expansion device driver attach detach bindings display oled lcd sensor peripheral"
packages_any = ["service_expansion"]
+++
# Expansion drivers and attached devices

Expansion drivers turn named buses and safe GPIO slots into active displays,
radios, sensors, or manual resource profiles. Drivers are package-gated, so the
available list depends on the firmware and board.

## Discover what is present

```text
expansion drivers
expansion devices
display list
```

From a script, inspect `solaros.expansion.drivers()` and
`solaros.expansion.devices()`. A driver existing in firmware does not mean a
physical device is attached.

## Attach deliberately

Use real bus and pin names returned by discovery:

```text
expansion attach pcd8544 lcd0 spi=spi0 cs=gpio10 dc=gpio4 reset=gpio5
display test lcd0
expansion detach lcd0
```

Detaching releases the resources. Do not invent a target name or copy bindings
from a different board.

## Quick reference

solaros.expansion.drivers() lists compiled drivers and devices() lists
currently attached devices with normalized bindings. attach(driver, name,
bindings) and detach(name) manage them. Never assume an example name such as
lcd0 or oled0 exists; inspect devices() or use a name explicitly supplied by
the user.
