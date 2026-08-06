+++
id = "media.input"
title = "Audio, keyboard input, and clipboard APIs"
section = "hardware"
summary = "Use installed media and input services"
aliases = ["audio", "ble", "clipboard"]
keywords = "python lua audio speaker microphone wav tone ble bluetooth keyboard clipboard"
packages_any = []
+++
# Audio, keyboard input, and clipboard APIs

These services are independent even though they are often used by foreground
applications. Inspect availability before calling an optional audio or BLE
operation.

## Keyboard input

Local hardware input uses structured press, release, and repeat events. The
input service tracks held keys by source and stable physical identity, while
legacy shell and text applications continue to receive translated characters.
BLE keyboards, fixed board buttons, `gpio-keys`, joysticks, and ADC D-pads share
one repeat policy. Configure it with `setterm keyrate`; the setting applies even
on a build without BLE.

Keyboard transports can additionally supply a canonical USB HID usage and
modifier mask. This keeps physical controls independent of the selected text
layout and provides the common representation needed by future keyboard buses.

## Audio

Use global volume unless a diagnostic or playback command explicitly needs an
override. Call `deinit()` or `off()` when a script owns output that should not
remain active.

Recording and playback require enough internal/DMA memory even on boards with
PSRAM. If an audio application reports no memory, stop unnecessary internal
stack jobs and inspect `mem`.

`tone_async()` queues a short tone and returns a request ID without waiting for
playback. Use `cancel()` with that ID or inspect `queue_status()` for the
current request and completed, cancelled, dropped, and failed counters. The
queue is bounded and shares exclusive output ownership with WAV playback and
native synth clients. A queued tone waits for that output; a full queue reports
an error to the caller.

## BLE keyboard

The BLE service manages one remembered keyboard and publishes its HID report
transitions through the generic input service. Pairing and scanning are system
operations; a script can inspect state and read translated key events.
`ble forget` erases the remembered keyboard from SolarOS NVS and removes its BLE
bond. On boards with a system KEY, a long press performs that forget operation
and then starts a new pairing scan. Pairing has no user cancellation path. The
KEY short-press power action remains separately configurable.

## Clipboard

The clipboard stores bounded text shared by applications. Clear sensitive
content after use.

## Quick reference

solaros.audio provides status, deinit or off, set_volume, set_mic_gain, tone,
tone_async, cancel, queue_status, level, loopback, wav_info, record_wav, and
play_wav. solaros.ble provides
status, connected, pair, forget, layout, read. solaros.clipboard provides set,
get, size, clear. Audio and BLE are package-gated.
