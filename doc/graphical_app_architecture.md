# Graphical touchscreen app architecture

Reference for building or updating a graphical (mouse + touch capable) SolarOS
app on the Waveshare ESP32-S3 RLCD boards, following the pattern established
by `ble_scanner`, `desibel`, `player`, `reader`, and `yazici`. Use this as the
checklist for new apps in this family.

## App lifecycle

Every app is a `solar_os_app_t` (`src/solar_os.h`): `start`/`suspend`/`resume`/
`stop` lifecycle callbacks plus one `event(ctx, event)` callback that handles
everything else. `solar_os_event_t.type` is one of:

- `SOLAR_OS_EVENT_CHAR` -- a plain keystroke (`event->data.ch`).
- `SOLAR_OS_EVENT_KEY` -- a non-char key (modifiers, function keys).
- `SOLAR_OS_EVENT_TICK` -- periodic tick, interval set by
  `solar_os_app_t.tick_interval_ms` (0 = default 25 ms).
- `SOLAR_OS_EVENT_RESUME` -- app regained foreground; re-render.
- `SOLAR_OS_EVENT_CLICK` -- `event->data.click.{x,y,buttons}`.
- `SOLAR_OS_EVENT_SCROLL` -- `event->data.scroll.{delta,x,y}`; delta sign is
  device-dependent, treat `delta < 0` as one direction consistently within
  the app.

**A graphical app is not done until it handles CLICK and SCROLL.** Keyboard
support alone is not sufficient -- every interactive element (buttons, list
rows, tabs) must be tappable, and every list/scrollable view longer than one
screen must respond to the mouse wheel. This was retrofitted into
`ble_scanner`'s device list after shipping keyboard-only navigation first;
build it in from the start on new apps instead.

### Shared draw/hit-test layout rule

Any time an app draws a clickable region (a list row, a custom button), the
pixel geometry used to draw it and the geometry used to hit-test it **must
come from the same function call**, never two hand-maintained copies. This is
exactly the class of bug the shared appbar component (below) was built to
avoid, and the same pattern was replicated locally in `ble_scanner` for its
device list (`ble_scanner_layout_list()` feeds both
`ble_draw_scanner_list()` and `ble_scanner_hit_test_list()`). Copy that
pattern for any app-specific clickable list or grid.

## Shared header/footer: `solar_os_appbar`

`src/services/solar_os_appbar.h/.c` is the standard chrome. Don't hand-draw a
header/footer bar or hand-type pixel constants for their height -- describe
*what* the bar shows and let the component handle layout, drawing, and hit
testing:

```c
solar_os_appbar_header_t header = {
    .title = "My App",
    .show_back = true,               /* false only for the launcher/home screen */
    .tabs = { .names = TAB_NAMES, .count = TAB_COUNT, .active_index = my_tab },
    .status_line = status_buf,       /* optional, or NULL */
};
solar_os_appbar_draw_header(gfx, &header);

solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
size_t n = build_footer_shortcuts(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = n };
solar_os_appbar_draw_footer(gfx, &shortcuts);
```

- Sizes (`solar_os_appbar_header_height/status_line_height/footer_height`)
  are proportional to `gfx` resolution, clamped to stay legible on small
  panels -- never hard-code a pixel height.
- Tabs (`count >= 2`) render as a **fixed-order strip**: tab order never
  changes based on which one is active, only the active tab's box is
  inverted (filled white / black text) to highlight it. Tabs and footer
  chips are separated by a "|" glyph so adjacent buttons read as distinct
  tap targets.
- On `SOLAR_OS_EVENT_CLICK`, call `solar_os_appbar_hit_test_header/_footer`
  with the exact same header/shortcuts struct used for drawing that frame.
  `SOLAR_OS_APPBAR_HIT_BACK`, `HIT_TAB_ITEM` (`.index`), and
  `HIT_FOOTER_ITEM` (`.index`) are the only outcomes; route `FOOTER_ITEM` to
  the same handler the matching keyboard shortcut already calls
  (`items[fhit.index].key`) so click and keyboard never diverge in
  behavior.

### Footer shortcut convention

Only list shortcuts that are **not** obvious -- Enter, Esc, and arrow-key
navigation are never included, every app already relies on those
universally. A chip's label is the function name only ("Save", "Connect"),
never the raw key, except Ctrl combos which render as `Ctrl+<KEY> Label`
since the modifier isn't otherwise discoverable from a bare label.

## No inline shortcut prose -- use the Help screen instead

Do not explain shortcuts with sentences drawn in the app body (e.g. a line
like `"[E] Edit  |  [H] Examples & Guide"` sitting in a settings panel). The
footer/header already carry short button labels, and now that every button
is directly tappable, spelling out key combinations in the body is
redundant and it wastes screen space that shrinks with every board's panel
size.

Instead, every app should expose one dedicated **Help** entry point:

- `Ctrl+H` in apps that accept free-text keyboard input (so a bare `H`
  keystroke isn't swallowed by a text field).
- Bare `H` in apps with no text-entry state (list/viewer/control-style
  apps, e.g. `ble_scanner`'s main list).

Help opens a full-screen scrollable reference (see `ble_scanner`'s
`ble_help_docs[]` / `showing_help` modal for the existing pattern) that is
the single place shortcuts, key combos, and usage notes are written out in
full. If an app already has such a Help screen, don't duplicate parts of it
elsewhere in the UI -- one copy, reachable from everywhere in the app.

## SD-only per-app data storage

`src/services/solar_os_storage.h`:

```c
esp_err_t solar_os_storage_app_data_path(const char *app_name, const char *leaf,
                                         char *path, size_t path_len);
esp_err_t solar_os_storage_app_data_dir(const char *app_name,
                                        char *path, size_t path_len);
```

Resolves under `<sd_mount_point>/.data/<app_name>/`, creating directories as
needed. Returns `SOLAR_OS_STORAGE_ERR_NO_SD_CARD` (== `ESP_ERR_NOT_FOUND`)
without ever falling back to internal flash when no SD card is mounted --
unlike `solar_os_storage_default_path()`/`resolve_path()`. Use this (not the
flash-fallback helpers) for any per-app settings, autosave, or position data
that should live on the SD card exclusively. On save, surface the no-SD-card
case as a status message instead of failing silently; on load, a missing
file and a missing SD card look the same to the user on first run, so no
warning is needed there.

## Build / flash quick reference

```bash
PYTHONUTF8=1 "$HOME/.platformio/penv/Scripts/pio.exe" run
PYTHONUTF8=1 PYTHONIOENCODING=utf-8 "$HOME/.platformio/penv/Scripts/python.exe" \
  -m esptool --chip esp32s3 --port COM7 --baud 460800 \
  write-flash 0x20000 .pio/build/waveshare_esp32_s3_rlcd_4_2/firmware.bin
```

`pio run -t upload`'s merge-bin step fails cosmetically (`ota_data_initial.bin`
offset overlap) after this project's partition table resize -- ignore that
one error line and flash `firmware.bin` directly at `0x20000` as shown above;
bootloader/partitions/ota_data_initial.bin do not need rewriting for
app-level changes. `PYTHONIOENCODING=utf-8` avoids a `UnicodeEncodeError`
crash from esptool's progress bar on a non-UTF-8 Windows console codepage.

## Checklist for a new/updated app in this family

- [ ] Uses `solar_os_appbar` for header/footer instead of hand-drawn chrome.
- [ ] Every clickable element's draw geometry and hit-test geometry come from
      one shared layout function.
- [ ] `SOLAR_OS_EVENT_CLICK` handled for every button/tab/list row.
- [ ] `SOLAR_OS_EVENT_SCROLL` handled for every view that can exceed one
      screen.
- [ ] No shortcut-explaining sentences in the app body; a Help screen
      (`Ctrl+H` or bare `H`) is the single source of shortcut documentation.
- [ ] Per-app persisted data uses `solar_os_storage_app_data_path/_dir`
      (SD-only), not the flash-fallback storage helpers.
