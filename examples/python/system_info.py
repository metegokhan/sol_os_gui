# SolarOS Text-Mode System Inspector
# Compatible with SolarOS MicroPython

import solaros

time = solaros.time
battery = solaros.battery
wifi = solaros.wifi
storage = solaros.storage

print("========================================")
print("     SOLAR-OS SYSTEM DIAGNOSTICS        ")
print("========================================")

# Battery Info
print("\n[BATTERY]")
bat = battery.status()
if bat:
    pct = bat["percent"]
    v_mv = bat["voltage_mv"]
    ext = "Yes" if bat["external_power"] else "No"
    print("Charge Level :", pct, "%")
    print("Voltage      :", v_mv, "mV")
    print("External Pwr :", ext)
else:
    print("Battery info unavailable")

# Wi-Fi Info
print("\n[WI-FI NETWORK]")
wf = wifi.status()
if wf:
    print("State        :", wf["state"])
    print("Connected    :", wf["connected"])
    if wf["connected"]:
        print("SSID         :", wf["ssid"])
        print("IP Address   :", wf["ip"])
        print("Signal (RSSI):", wf["rssi"], "dBm")
else:
    print("Wi-Fi info unavailable")

# Uptime Info
print("\n[UPTIME]")
uptime_ms = time.uptime_ms()
uptime_s = uptime_ms // 1000
print("Uptime (ms)  :", uptime_ms)
print("Uptime (sec) :", uptime_s, "seconds")

# Storage Info
print("\n[STORAGE]")
if storage.is_mounted():
    mount = storage.mount_point()
    print("Mount Point  :", mount)
    usage = storage.usage()
    if usage:
        print("Total Bytes  :", usage["total_bytes"])
        print("Free Bytes   :", usage["free_bytes"])
else:
    print("SD Storage   : Not Mounted")

print("\n========================================")
print("Diagnostics Complete.")
