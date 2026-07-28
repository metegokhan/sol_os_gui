# Unified device manual

SolarOS keeps user help and agent API guidance in one canonical source:
`doc/manual/*.md`. Each topic is an ordinary Markdown tutorial with TOML
frontmatter and a final `Quick reference` section.

The build generator reads those pages and creates a package-gated C registry.
`man` displays a terminal-normalized form of the tutorial, while the agent's
`solaros_reference` tool returns the compact `Quick reference` section. Topic
IDs, aliases, summaries, keywords, and package gates therefore cannot drift
between the two interfaces.

## Topic format

```markdown
+++
id = "python.gfx"
title = "Python graphics"
section = "api"
summary = "Draw safely on the active display"
aliases = ["gfx"]
keywords = "python graphics display"
packages_any = ["app_python"]
+++
# Python graphics

Tutorial text...

## Quick reference

The concise, authoritative contract used by the agent...
```

The `id` is the runtime lookup key, and the filename must be `<id>.md`. IDs and
aliases must be unique. `packages_any` contains package IDs from
`packages/solar_os_packages.toml`. A topic is embedded when any named package is
present; an empty list makes it universal.

## Release and refresh

The deployment pipeline copies the Markdown tree to
`/ota/<version>/doc/manual/`, generates `doc/catalog.json`, and signs the exact
catalog bytes as `doc/catalog.sig` with the OTA release key. Each catalog entry
contains the page path, byte count, SHA-256, metadata, and Quick reference.

On a Wi-Fi, PSRAM, and SD capable build, `docs update` resolves the configured
OTA URL to the exact running firmware version. The device verifies the catalog
signature, rejects version mismatches, downloads every page into an immutable
revision directory, and checks its signed size and SHA-256 before changing the
small active-revision pointer. Existing readers retain valid paths because
activated revisions are not deleted at runtime.

Downloaded pages override only the body and Quick reference of topics already
compiled into the firmware. Search metadata and package availability remain
the embedded registry's responsibility. If the SD card, active pointer,
signature, catalog, page, or parser is unavailable, each lookup falls back to
the embedded copy.

Use:

```text
docs status
docs update
docs reset
```

`docs reset` removes the active pointer and immediately restores the embedded
manual. Cached immutable revision directories may remain on SD.
