+++
id = "docs"
title = "Browsing and refreshing documentation"
section = "service"
summary = "Browse the manual and refresh its signed Markdown pages"
aliases = ["documentation"]
keywords = "docs documentation manual browser tui update refresh signed catalog sd fallback version"
packages_any = ["app_docs"]
+++
# Browse and refresh documentation

SolarOS always carries a manual in firmware, so `man` and the agent reference
tool work without a network connection.

Run `docs` to open the foreground documentation browser. Use the arrow keys to
select a topic and Enter to read it. The topic opens through the same rendered
manual path as `man TOPIC`, so TOML frontmatter and Markdown syntax are not
shown as document content.

## Refresh from solar-os.eu

On devices with Wi-Fi, PSRAM, and an SD card, the same manual can be refreshed
without installing new firmware.

First connect Wi-Fi and make sure the SD card is mounted. Then run:

```text
docs update
```

SolarOS requests the documentation published for its exact running firmware
version. It verifies the catalog with the OTA public key and verifies every
Markdown page against the signed size and SHA-256 before activating the new
revision. The command shows catalog and per-page download progress. An
interrupted or invalid download leaves the previous manual active.

`docs status` shows whether the external revision or embedded fallback is in
use. `docs reset` stops using the downloaded revision; it does not remove the
immutable cached files from the SD card.

## Why versions must match

Documentation can affect scripts produced by the agent. A page for a newer
firmware might describe APIs that do not exist on the running device. SolarOS
therefore rejects catalogs whose firmware version differs, even if their
signature is valid.

## Quick reference

`docs` opens the topic browser; `docs TOPIC` selects a topic initially.
`docs status` reports the active source, firmware version, revision, page count,
update state, and last error. `docs update` displays download progress, stores
the signed exact-version catalog and Markdown pages on SD, and activates them
only after signature, size, and SHA-256 verification. `docs reset` immediately
returns man, docs, and the agent to the embedded manual. Refreshing requires
Wi-Fi, PSRAM, and SD.
