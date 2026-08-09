+++
id = "packages"
title = "Firmware packages and flavors"
section = "build"
summary = "Understand package ownership, groups, flavors, and custom builds"
aliases = ["flavors"]
keywords = "packages flavors groups capabilities build custom firmware pkg"
packages_any = []
+++
# Firmware Packages and Flavors

SolarOS package selection is declared in `packages/solar_os_packages.toml`.
Flavor files select groups or individual packages; the generator resolves
package dependencies and then removes packages unsupported by the target board.

`service.streams` owns the dynamic typed endpoint registry. Sensor, port, and
audio providers register their endpoints there at runtime. `service.audio`
also owns audio-device discovery; devices refer to their capture and playback
stream IDs instead of exposing a board-specific global data path.

## Ownership Rules

- `bootstrap` is immutable and contains only the runtime and shell needed to
  start SolarOS. A flavor cannot disable its members.
- Groups are selection shortcuts only. They cannot own source files or ESP-IDF
  component requirements.
- Every source file and component requirement belongs to a package.
- A package lists other packages it needs with `depends`. Enabling an app or job
  automatically enables its transitive dependencies. Explicitly disabling a
  required package is an error.
- Board capability pruning is applied to the resolved graph. If a dependency is
  unavailable, its dependants are removed as well.

The standard selectors are `system`, `expansions`, `maintenance_apps`,
`maintenance_jobs`, `hardware_jobs`, `audio`, `net`, `agent`, `media`, `games`,
`retro`, `python`, `lua`, `writing`, and `utils`. Maintenance jobs contain
background logging and battery monitoring. Hardware jobs contain Bridge, DAQ,
GPIO Keys, and SUMP for hardware diagnostics and hacking. The `writing` group contains
Reader, Writer, Files, and Notes; general utilities contain Clock, Calculator,
Plot, Logic, and Sheet.

The `retro` flavor is the full firmware plus experimental emulation packages.
Its `retro` group currently selects `app.gameboy`, which requires graphics,
PSRAM, and SD storage. When `service.synth` is present, Game Boy also uses the
MiniGB APU through the shared synth and audio services.

The `writerdeck` flavor targets the Elecrow e-paper board with the `writing`
group plus system, maintenance, and network tools. It excludes general
utilities and hardware-diagnostics jobs to stay focused and fit the board's
smaller OTA slot.

The `rover`, `rover-python`, and `rover-lua` flavors target the Freenove
ESP32-WROVER v3.0 composite-video terminal. All three include the expansion
framework and drivers, networking, media viewing, general utilities, the
writing suite, the log job, Bridge, and the GPIO Keys job. They omit the battery monitor
because the board has no battery hardware, and omit the DAQ and SUMP jobs by
default. The Logic app is also omitted because its timing-sensitive capture
buffer requires more internal-memory margin than this configuration provides.
`rover` includes games and has no embedded interpreter;
`rover-python` and `rover-lua` omit games and add only their selected scripting
stack. Agent remains excluded because its runtime memory requirements exceed
the practical internal-memory margin. The board's 4 MB flash uses one large
factory application slot, so these flavors omit OTA and remote manual
synchronization. The embedded `docs` application remains available.

`rover-retro` is a focused Freenove Game Boy build with the normal system
service baseline. It includes BLE keyboard input, SD storage, UART ports,
hardware I/O services, filesystem commands, Docs, Edit, Less, Com, Files, the
log job, Bridge, GPIO Keys, Wi-Fi, and the SSH/SCP clients. It excludes the remaining
network stack, expansion drivers, media, the rest of the writing suite,
utilities, other games, scripting, and OTA. The Freenove board has no usable
audio backend while
composite scanout owns I2S0, so Game Boy runs without MiniGB APU or synth output
in this flavor.

Network ownership is intentionally split. `network.base`, `network.mqtt`,
`network.ssh`, `network.mail`, `messaging.gateway`, `network.http-client`, and
`network.http-server` own their individual implementations. Image and document
decoding are separate `media.image` and `media.document` packages, so selecting
`app.curl`, for example, does not pull MQTT, SSH, mail, or image dependencies.

`network.http-client` owns the shared TLS-enabled HTTP transport used by `curl`
and `web`. It exposes request headers and bodies, redirects, streaming response
events, cross-task cancellation, per-I/O timeouts, and an end-to-end deadline.
Callers continue to own their worker task and response consumer; see
[HTTP Client Service](../http_client.md) for the native API and lifecycle.

The `agent` group selects `app.agent` and its `service.agent` dependency.
`service.agent` owns provider-neutral events, NVS-backed provider
configuration, bounded tool-loop policy, a declarative typed-tool registry,
and the OpenAI Responses/Chat-Completions adapter. The registry owns provider
schemas, output schemas, risk and availability metadata, and shared execution
for the system, storage, jobs, and policy-gated script tools. Its NVS-backed
`off`, `readonly`, `confirm`, and `all` policy filters advertised schemas and
is enforced again at execution time.
It depends on the shared HTTP and JSON services and is pruned from targets
without both Wi-Fi and PSRAM. Python and Lua are not dependencies. See
[Native Agent Service](agent.md).

`app.python` and `app.lua` each depend on `service.script_runner`. That service
defines the common source/file request, bounded output, cancellation, deadline,
and completion-status contract. Each interpreter owns its language adapter and
single-owner guard. `app.agent` supplies installed adapters to both the manual
`agent script` command and the typed agent registry without making either
interpreter a dependency of the agent package.

Messaging is split into explicit layers: `service.messaging` owns
provider-neutral conversations, history, delivery state, and the pending
outbox; `messaging.gateway` owns gateway configuration and room commands;
`chat.transport.gateway` owns the gateway wire protocol; and
`job.gateway-sync` owns connection lifetime, retries, cursors, and gateway
delivery. The bounded store persists full messages on SD and uses the
Inbox's compact persistent records as its internal-flash fallback. Stable
producer IDs suppress transport replays before they reach either UI. `app.chat`
depends only on the provider-neutral Messaging service and is a foreground view
over that shared state; `job.chatd` remains the
independent local gateway server.

Inbox storage and presentation are also separate. Producers such as mail and
POCSAG depend only on `service.inbox`; it owns the bounded persistent ring under
`/.inbox/`, durable read state, producer-key replay suppression, and the
persisted opt-in notification-sound policy. Audio-capable builds enqueue the
sound through `service.audio`; Inbox remains available without audio hardware.
`app.inbox` adds the foreground browser and its shell command.

`service.synth` depends on `service.audio` and provides exclusive real-time PCM
rendering for reusable synthesizers and emulated sound hardware. The audio
service retains codec, global-volume, and output-serialization ownership; the
synth service owns a bounded render block, dedicated worker, client ownership,
and deadline/error counters. Native apps can supply an independent signed
16-bit stereo render callback.

## Custom Flavor Example

This flavor adds only `curl` and the dependency closure needed by that app to
the immutable bootstrap:

```toml
[flavor]
name = "curl-only"
description = "Bootstrap plus the HTTP client app."

[packages]
app_curl = true
```

Use `pkg` on the device to inspect the resolved package list.

## Quick reference

Packages are the actual build units, groups are convenience bundles, and a
flavor selects packages for a board. Board capabilities remove packages that
cannot run. Use `pkg` on-device to inspect the resolved firmware and edit a
flavor TOML file when producing a custom build.
