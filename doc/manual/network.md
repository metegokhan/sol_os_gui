+++
id = "network"
title = "Wi-Fi, MQTT, and network APIs"
section = "network"
summary = "Connect, inspect, and communicate over installed network services"
aliases = ["wifi", "mqtt", "net"]
keywords = "python lua wifi wireless station access point ap nat scan connect mqtt network ping"
packages_any = ["service_wifi", "service_mqtt", "service_net"]
+++
# Wi-Fi, MQTT, and network APIs

Network modules are package-gated. Inspect their status before assuming Wi-Fi,
MQTT, or diagnostic networking exists in the current firmware.

## Wi-Fi

Wi-Fi is enabled by default. `wifi disable` prevents the Wi-Fi driver and its
station/AP network interfaces from initializing on the next boot. `wifi enable`
enables them again for the next boot. Both commands leave the current boot and
saved network profiles unchanged. `wifi on` and `wifi off` remain live radio
controls for the current boot.

The `espnow-link` job uses a connectionless Wi-Fi lease. In automatic mode it
follows the active station or AP channel and otherwise uses channel 6. While
the lease is active, scanning is rejected. A fixed ESP-NOW channel also rejects
a new station connection or an AP configured for another channel. `wifi off`
turns off station/AP networking but reports that the radio remains active until
the ESP-NOW job stops.

From the shell, `wifi` opens the display TUI and `wifi status` works on every
shell. A script can scan before connecting:

```python
import solaros

for network in solaros.wifi.scan():
    print(network)
print(solaros.wifi.status())
```

Connecting or stopping Wi-Fi can interrupt an active agent, SSH, chat, or HTTP
session. Confirm disruptive changes locally.

## MQTT

Connect to a broker, subscribe, then read messages with bounded timeouts. MQTT
settings are stored by the service; do not embed credentials in a public
script.

## Quick reference

solaros.wifi provides status, status_text, start, stop, connect, connect_saved,
disconnect, forget, forget_ssid, forget_all, known, scan, ap_start, ap_stop,
and nat. solaros.mqtt provides status, connect, disconnect, publish, subscribe,
and read. solaros.net.ping(host, optional count, timeout_ms, interval_ms,
data_size) returns statistics. These modules are package-gated.
