+++
id = "sessions.apps"
title = "Foreground sessions and applications"
section = "service"
summary = "Create shells and inspect resumable foreground applications"
aliases = ["sessions", "apps"]
keywords = "python lua sessions apps shell port terminal create close registry foreground"
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

`Alt+Tab` switches between display sessions. Closing an application returns to
a clean shell prompt.

## Create another shell

```text
session create shell cdc0 --term auto
session create shell uart0 --term vt100 --size 100x30
session create shell lcd0
```

Use `port list` or `display list` to discover real targets first. A manually
created port session does not rerun `/.shell/startup`.

## Quick reference

solaros.sessions.create_shell(port, optional term, cols, rows) returns a
session id; close(id) closes it. Script-created port shells do not run
/.shell/startup. solaros.apps.list() and find(name) inspect registered
foreground apps.
