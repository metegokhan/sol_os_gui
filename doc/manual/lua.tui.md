+++
id = "lua.tui"
title = "Lua text user-interface API"
section = "api"
summary = "Build terminal applications from Lua"
aliases = []
keywords = "lua tui terminal text interface curses keyboard keys input box bold inverse"
packages_any = ["app_lua"]
+++
# Lua text user-interface API

Use `solaros.tui` for text applications that should share one interface across
the display terminal and cursor-addressable port shells.

## Minimal application

```lua
local tui = solaros.tui

while not solaros.should_exit() do
    tui.clear()
    tui.move(0, 0)
    tui.addstr("SolarOS TUI", tui.BOLD)
    tui.move(2, 0)
    tui.addstr("Press the app-exit key to leave")
    tui.refresh()
    tui.getch()
end
```

Use `rows`, `cols`, or `size` to adapt the layout. Check
`solaros.should_exit()` so the foreground session can close cleanly.

## Quick reference

Lua: local tui = solaros.tui. Functions are rows, cols, size, clear, refresh,
move, write, addstr, putch, hline, vline, vrule, box, fill, and getch.
Attributes are NORMAL, BOLD, INVERSE; key constants include KEY_ESCAPE and
navigation keys. Loop while not solaros.should_exit() for an interactive
foreground app.
