# SolarOS Graphical Digital Clock
# Compatible with SolarOS MicroPython

import solaros

gfx = solaros.gfx
time = solaros.time


def draw_big_digit(x, y, char):
    # Renders bold clean characters
    gfx.font(gfx.FONT_BOLD_20)
    gfx.text(x, y, char)


def main():
    gfx.begin()
    w = gfx.width()
    h = gfx.height()

    while True:
        gfx.clear(gfx.WHITE)
        gfx.color(gfx.BLACK)

        # Header
        gfx.font(gfx.FONT_BOLD)
        gfx.text(20, 30, "SOLAR-OS DIGITAL CLOCK")
        gfx.line(20, 38, w - 20, 38)

        # Time Info
        dt = time.datetime()
        if dt:
            year = dt.get("year", 2026)
            month = dt.get("month", 1)
            day = dt.get("day", 1)
            hour = dt.get("hour", 0)
            minute = dt.get("minute", 0)
            second = dt.get("second", 0)

            # Date String
            d_str = ("0" if day < 10 else "") + str(day) + "/" + ("0" if month < 10 else "") + str(month) + "/" + str(year)
            gfx.font(gfx.FONT_MONO_14)
            gfx.text(60, 80, "Date: " + d_str)

            # Big Clock Box
            gfx.rect(40, 110, 320, 90)
            t_str = ("0" if hour < 10 else "") + str(hour) + " : " + ("0" if minute < 10 else "") + str(minute) + " : " + ("0" if second < 10 else "") + str(second)
            gfx.font(gfx.FONT_BOLD_20)
            gfx.text(60, 165, t_str)
        else:
            # Fallback to uptime clock if RTC is unset
            up_s = time.uptime_ms() // 1000
            m = (up_s % 3600) // 60
            s = up_s % 60
            hr = up_s // 3600
            t_str = ("0" if hr < 10 else "") + str(hr) + ":" + ("0" if m < 10 else "") + str(m) + ":" + ("0" if s < 10 else "") + str(s)
            gfx.font(gfx.FONT_MONO_14)
            gfx.text(60, 80, "System Timer Clock:")
            gfx.rect(40, 110, 320, 90)
            gfx.font(gfx.FONT_BOLD_20)
            gfx.text(80, 165, t_str)

        # Footer
        gfx.font(gfx.FONT_SMALL)
        gfx.line(20, h - 30, w - 20, h - 30)
        gfx.text(20, h - 12, "Press [ESC] to return to Launcher")

        gfx.present()

        # Sleep & Poll
        key = gfx.getch(1000)
        if key == 27 or key == 113 or key == 81:
            break

    gfx.end()


main()
