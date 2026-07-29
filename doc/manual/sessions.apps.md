+++
id = "sessions.apps"
title = "Foreground sessions and applications"
section = "service"
summary = "Create shells and inspect resumable foreground applications"
aliases = ["sessions"]
keywords = "python lua sessions apps shell port terminal create close focus input ble registry foreground"
packages_any = []
+++
# Foreground sessions and applications

A session is a foreground application or shell attached to a display or byte
stream. Sessions let you switch away from an application and return without
discarding its state.

## Inspect and switch

```text
session list
session fg 3
session close 3
```

`Alt+Tab` switches between sessions on the locally focused display. `session
fg` explicitly foregrounds its session and moves local input focus to that
session's display. Closing an application returns to that display's shell.

## Create another shell

```text
session create shell cdc0 --term auto
session create shell uart0 --term vt100 --size 100x30
session create shell lcd0
```

Use `port list` or `display list` to discover real targets first. A manually
created port session does not rerun `/.shell/startup`. A display shell is
created on its target without changing local input focus; use `session focus`
when the BLE keyboard or board controls should move to it.

## Start an app on a display

An app can be launched onto a named display from any shell:

```text
session create files display0
session create reader oled0 /manual.md
```

The invoking port shell immediately returns to its own prompt. The new app
receives input belonging to its display; serial input remains with the serial
shell. App arguments use the invoking shell's current directory for the same
path-aware apps as a direct launch.

## Local input focus

BLE keyboard, board buttons, joystick, and D-pad input share one local display
focus. The primary board display is selected at boot. On a headless board, the
first display shell created becomes the default.

```text
session focus
session focus oled0
```

Changing focus does not move or restart a session. It only assigns local input
and target-scoped `Alt+Tab`. Browser input from `displayd` remains assigned to
the display being controlled.

## Quick reference

solaros.sessions.create_shell(port, optional term, cols, rows) returns a
session id; close(id) closes it. Script-created port shells do not run
/.shell/startup. solaros.apps.list() and find(name) inspect registered
foreground apps.
