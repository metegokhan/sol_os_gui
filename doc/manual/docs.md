+++
id = "docs"
title = "Refreshing the device manual"
section = "service"
summary = "Download and activate signed documentation for this firmware version"
aliases = ["documentation"]
keywords = "docs documentation manual update refresh signed catalog sd fallback version"
packages_any = ["service_docs"]
+++
# Refreshing the device manual

SolarOS always carries a manual in firmware, so `man` and the agent reference
tool work without a network connection. On devices with Wi-Fi, PSRAM, and an SD
card, the same manual can be refreshed without installing new firmware.

First connect Wi-Fi and make sure the SD card is mounted. Then run:

```text
docs update
```

SolarOS requests the documentation published for its exact running firmware
version. It verifies the catalog with the OTA public key and verifies every
Markdown page against the signed size and SHA-256 before activating the new
revision. An interrupted or invalid download leaves the previous manual active.

`docs status` shows whether the external revision or embedded fallback is in
use. `docs reset` stops using the downloaded revision; it does not remove the
immutable cached files from the SD card.

## Why versions must match

Documentation can affect scripts produced by the agent. A page for a newer
firmware might describe APIs that do not exist on the running device. SolarOS
therefore rejects catalogs whose firmware version differs, even if their
signature is valid.

## Quick reference

`docs status` reports the active source, firmware version, revision, page count,
update state, and last error. `docs update` downloads the signed exact-version
catalog and Markdown pages to SD and activates them only after signature, size,
and SHA-256 verification. `docs reset` immediately returns man and the agent to
the embedded manual. The feature requires Wi-Fi, PSRAM, and SD.
