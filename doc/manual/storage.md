+++
id = "storage"
title = "Storage and shell paths"
section = "shell"
summary = "Use SolarOS volumes, files, directories, and shell-style paths"
aliases = ["filesystem", "files"]
keywords = "python lua storage filesystem files directories mount sd flash copy rename remove mkdir path disk volume"
packages_any = []
+++
# Storage and shell paths

SolarOS presents the default storage volume as `/`. On an SD-backed target this
normally means the SD card, while `/flash` remains the internal flash volume.
On a board without SD, `/` normally maps to internal flash.

Shell paths are not host operating-system paths. Scripts should use
`solaros.storage` so the same code follows SolarOS mount and path rules.

## Inspect before writing

```python
import solaros

print(solaros.storage.status())
print(solaros.storage.blocks())
print(solaros.storage.usage())
```

Use `resolve(path)` when a native or library operation needs the resolved
internal path. Check free space before copying or producing a large capture.

## Volumes and directories

The storage API can create and remove directories, copy or rename files, and
mount detected volumes. Destructive calls report SolarOS errors; do not assume
that a failed operation partially succeeded.

## Quick reference

Use solaros.storage, not host os or io APIs. Functions include status,
is_mounted, mount, unmount, mount_point, usage, resolve, rescan, blocks,
block_count, block, usage_for_block, mkdir, rmdir, remove, rename, copy,
mount_volume, and unmount_volume. SolarOS shell paths use slash for the active
default storage volume.
