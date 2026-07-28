+++
id = "python.tui"
title = "Python text user-interface API"
section = "api"
summary = "Build terminal applications from MicroPython"
aliases = ["py.tui"]
keywords = "python py tui terminal text interface curses keyboard keys input box bold inverse"
packages_any = ["app_python"]
+++
# Python text user-interface API

Use `solaros.tui` for an interactive text application that should work on both
the built-in display terminal and cursor-addressable port shells. It supplies
screen dimensions, styled text, boxes, key input, and refresh control.

## Minimal application

```python
import solaros
from solaros import tui

while not solaros.should_exit():
    tui.clear()
    tui.move(0, 0)
    tui.addstr("SolarOS TUI", tui.BOLD)
    tui.move(2, 0)
    tui.addstr("Press the app-exit key to leave")
    tui.refresh()
    tui.getch()
```

Read `rows()` and `cols()` instead of assuming terminal dimensions. Keep the
loop cooperative by checking `solaros.should_exit()`.

## Quick reference

Python: from solaros import tui. Functions are rows, cols, size, clear, refresh,
move, write, addstr, putch, hline, vline, vrule, box, fill, and getch.
Attributes are NORMAL, BOLD, INVERSE; key constants include KEY_ESCAPE and
navigation keys. Loop while not solaros.should_exit() for an interactive
foreground app.
