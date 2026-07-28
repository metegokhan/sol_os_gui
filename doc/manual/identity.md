+++
id = "identity"
title = "Device identity"
section = "service"
summary = "Read and configure the NVS-backed user and hostname"
aliases = ["hostname", "user"]
keywords = "python lua identity hostname user username nvs wifi advertised device name"
packages_any = []
+++
# Device identity

The device user and hostname are stored in NVS, so they do not depend on an SD
card. The shell prompt uses both values, and network services use the hostname
when advertising the device.

## Inspect and change identity

From the shell:

```text
identity status
identity user nils
identity hostname solarterm
```

From Python:

```python
import solaros

print(solaros.identity.format())
solaros.identity.set_hostname("solarterm")
```

If Wi-Fi is already initialized, reboot before expecting every advertisement or
DHCP hostname to use a changed value.

## Quick reference

solaros.identity provides user(), hostname(), set_user(name),
set_hostname(name), and format(). Values are NVS-backed. Reboot before
expecting an already initialized Wi-Fi interface to advertise a changed
hostname.
