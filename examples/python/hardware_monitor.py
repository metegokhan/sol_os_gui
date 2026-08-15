# SolarOS Graphical Hardware Monitor
# Compatible with SolarOS MicroPython

import solaros

gfx = solaros.gfx
time = solaros.time
battery = solaros.battery
wifi = solaros.wifi


def draw_bar(x, y, w, h, percent):
    if percent < 0:
        percent = 0
    if percent > 100:
        percent = 100
    gfx.color(gfx.BLACK)
    gfx.rect(x, y, w, h)
    fill_w = (w - 4) * percent // 100
    if fill_w > 0:
        gfx.fill_rect(x + 2, y + 2, fill_w, h - 4)


def main():
    gfx.begin()
    w = gfx.width()
    h = gfx.height()

    while True:
        gfx.clear(gfx.WHITE)
        gfx.color(gfx.BLACK)

        # Header
        gfx.font(gfx.FONT_BOLD)
        gfx.text(15, 25, "SOLAR-OS HARDWARE MONITOR")
        gfx.line(15, 32, w - 15, 32)

        # 1. Battery Section
        gfx.font(gfx.FONT_BOLD)
        gfx.text(20, 55, "[ BATTERY ]")
        gfx.font(gfx.FONT_SMALL)
        bat = battery.status()
        if bat:
            pct = bat["percent"]
            v_mv = bat["voltage_mv"]
            v_int = v_mv // 1000
            v_dec = (v_mv % 1000) // 10
            ext = "Charging" if bat["external_power"] else "Battery"

            bat_str = str(pct) + "%  (" + str(v_int) + "." + ("0" if v_dec < 10 else "") + str(v_dec) + "V) - " + ext
            gfx.text(35, 75, bat_str)
            draw_bar(35, 83, 200, 12, pct)
        else:
            gfx.text(35, 75, "Battery status unavailable")

        # 2. Wi-Fi Section
        gfx.font(gfx.FONT_BOLD)
        gfx.text(20, 120, "[ WI-FI NETWORK ]")
        gfx.font(gfx.FONT_SMALL)
        wf = wifi.status()
        if wf and wf["connected"]:
            ssid = wf["ssid"]
            ip = wf["ip"]
            rssi = wf["rssi"]
            gfx.text(35, 140, "Status: Connected to " + ssid)
            gfx.text(35, 155, "IP Addr: " + ip + "   Signal: " + str(rssi) + " dBm")
            rssi_pct = 100 + rssi if rssi > -100 else 0
            draw_bar(35, 163, 200, 10, rssi_pct)
        elif wf and wf["started"]:
            gfx.text(35, 140, "Status: Scanning / Disconnected")
        else:
            gfx.text(35, 140, "Status: Wi-Fi Radio OFF")

        # 3. System Uptime Section
        gfx.font(gfx.FONT_BOLD)
        gfx.text(20, 195, "[ SYSTEM RUNTIME ]")
        gfx.font(gfx.FONT_SMALL)
        uptime_s = time.uptime_ms() // 1000
        hrs = uptime_s // 3600
        mins = (uptime_s % 3600) // 60
        secs = uptime_s % 60
        up_str = "Uptime: " + str(hrs) + "h " + str(mins) + "m " + str(secs) + "s (" + str(uptime_s) + " seconds total)"
        gfx.text(35, 215, up_str)

        # Footer
        gfx.line(15, h - 25, w - 15, h - 25)
        gfx.font(gfx.FONT_SMALL)
        gfx.text(20, h - 10, "Press [ESC] or [Q] to return to Launcher")

        gfx.present()

        # Check key input (wait up to 1000ms before refreshing)
        key = gfx.getch(1000)
        if key == 27 or key == 113 or key == 81:  # ESC, 'q', 'Q'
            break

    gfx.end()


main()
