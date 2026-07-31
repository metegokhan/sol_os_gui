+++
id = "expansion"
title = "Expansion drivers and attached devices"
section = "hardware"
summary = "Discover, attach, and detach package-gated expansion devices"
aliases = ["devices", "drivers", "rfm69", "rfm95", "neopixel", "ws2812", "lora", "fsk", "gfsk", "msk", "gmsk", "ook"]
keywords = "python lua expansion device driver attach detach bindings display oled lcd sensor peripheral radio rfm69 rfm95 neopixel ws2812 rgb led strip fsk gfsk msk gmsk ook lora"
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

An RFM95W wired to the ESP32-S3-DevKitC-1 `spi0` bus with NSS on GPIO4 and
reset on GPIO5 attaches as a multimode packet radio:

```text
expansion attach rfm95 radio0 spi=spi0 cs=gpio4 reset=gpio5
radio status radio0
radio profile apply radio0 lora-eu868
```

Connect an antenna suitable for the module band before transmitting. See the
expansion reference for the complete wiring and modulation configuration.

A WS2812/NeoPixel strip uses one runtime-safe GPIO and a declared pixel count:

```text
expansion attach neopixel pixels0 data=gpio1 count=8
neopixel fill pixels0 16 0 0
neopixel set pixels0 3 0 16 0
neopixel clear pixels0
expansion detach pixels0
```

SolarOS stores colors in RGB form and transmits the strip's standard GRB wire
order. Attach clears all declared pixels. `set` and `fill` refresh immediately
in the shell; scripting APIs buffer changes until `show()`.

## Quick reference

solaros.expansion.drivers() lists compiled drivers and devices() lists
currently attached devices with normalized bindings. attach(driver, name,
bindings) and detach(name) manage them. Never assume an example name such as
lcd0 or oled0 exists; inspect devices() or use a name explicitly supplied by
the user.
