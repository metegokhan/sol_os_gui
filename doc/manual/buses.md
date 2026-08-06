+++
id = "buses"
title = "Named runtime buses"
section = "api"
summary = "Create and use resource-owned I2C, SPI, UART, OneWire, and PS/2 buses"
aliases = ["bus"]
keywords = "python lua buses resource named bus create attach detach i2c spi uart onewire ps2 keyboard lease"
packages_any = ["service_resources"]
+++
# Named runtime buses

Named buses let scripts and expansion drivers share one description of a
physical I2C, SPI, UART, OneWire, or PS/2 connection. The registry records pins,
readiness, sharing policy, and active leases.

## Inspect before creating

```python
import solaros

for bus in solaros.buses.list():
    print(bus)
```

Reuse a suitable registered bus instead of creating a duplicate on the same
pins. Use `attach` and `detach` for leases, and remove only a runtime bus that
has no users.

## Transfer through the registry

Use the family matching the descriptor: `i2c_scan`, `spi_xfer`, `uart_read`, or
`onewire_scan`. Inspect the result and release any lease even when a transfer
fails.

## Quick reference

Prefer solaros.buses for named hardware. list and get inspect registered buses;
create_i2c, create_onewire, create_ps2, create_spi, and create_uart create runtime buses;
attach, detach, remove manage lifecycle. Transfer families are i2c_probe or
scan or read_reg or write_reg, onewire_reset or scan or xfer, spi_xfer or read
or write, and uart_read or write. Inspect descriptors before choosing names,
pins, chip selects, or hosts.
