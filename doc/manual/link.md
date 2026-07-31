+++
id = "link"
title = "SolarOS Link"
section = "service"
summary = "Transport-independent packet messaging and the radio-link adapter"
aliases = ["radio-link", "link.protocol"]
keywords = "link packet radio text binary acknowledgement broadcast queue protocol crc duplicate"
packages_any = ["service_link", "job_radio_link"]
+++
# SolarOS Link

SolarOS Link is a small transport-independent message layer for packet-sized
connections. The Link service owns framing, protocol CRC, sequence numbers,
acknowledgements, duplicate suppression, and bounded receive/transmit queues.
A transport adapter moves complete Link frames over a specific medium.

Version one deliberately does not provide routing, fragmentation, encryption,
or mesh forwarding. One frame must fit the selected transport MTU. Use it for
small local text and binary messages, not arbitrary files or trusted secrets.

## Radio Quick Start

Attach a packet radio, then start the adapter with a complete radio profile:

```text
expansion attach rfm95 radio0 spi=spi0 cs=gpio4 reset=gpio5
job start radio-link link0 radio0 lora-eu868
link status link0
link send link0 broadcast "hello"
```

To copy received text messages into the universal inbox:

```text
job start radio-link link0 radio0 lora-eu868 inbox=on
```

`inbox=off` is the default. Accepted text and binary messages remain available
through the bounded Link receive queue in either mode; `inbox=on` additionally
publishes accepted text messages to the inbox. Read one queued message with:

```text
link receive link0
link receive link0 1000
```

The optional timeout is in milliseconds and is limited to 1000 in the shell.

To use Link text in the unified Chat and Messages interfaces instead:

```text
job start radio-link link0 radio0 lora-eu868 chat=on
chat
```

Chat shows a `link` provider section with a `link0 broadcast` conversation.
Each received 32-bit source ID creates a discovered Contact and direct
conversation. Rename, trust, or block it with the normal `contacts` commands.
The Chat projection observes accepted frames without consuming the Link receive
queue, so `link receive` remains available. `chat=on` and `inbox=on` are
mutually exclusive because generic messaging already publishes received Chat
messages to Inbox.

## Commands

| Command | Description |
| --- | --- |
| `link status\|list` | List active Link instances and their queue/protocol counters. |
| `link status <link>` | Show one Link instance, local ID, MTU, queue depths, ACK state, duplicates, CRC errors, and drops. |
| `link send <link> <broadcast\|destination-id> <text>` | Queue one text message. Unicast requests an acknowledgement. |
| `link send-binary <link> <broadcast\|destination-id> <byte...>` | Queue one binary message from decimal or `0x` byte values. |
| `link receive <link> [timeout-ms]` | Remove and print the oldest accepted text or binary message. |

Destination IDs accept decimal or `0x` notation. `broadcast` is the reserved
destination `0xffffffff`. `link status` prints this device's stable 32-bit
local ID as hexadecimal; it is derived from the ESP32 base MAC.

The displayed frame MTU comes from the active transport and radio profile. The
payload limit is the MTU minus the 12-byte header and 2-byte protocol CRC. For
example, the 255-byte LoRa packet profile carries at most 241 Link payload
bytes, while a 64-byte FSK profile carries at most 50.

## radio-link Job

Usage:

```text
job start radio-link <link> <radio> <profile> [inbox=off|on] [chat=off|on]
job status radio-link
job stop radio-link
```

The job claims the radio as `job:radio-link`, applies the named radio profile,
creates the Link instance, and continuously alternates queued transmission and
packet reception. While it runs, mutating `radio config`, `radio state`,
`radio send`, `radio recv`, and profile-apply operations are rejected; read-only
radio status remains available and shows the owner.

Stopping the job destroys its Link queues, restores the radio configuration and
state that existed at startup, and releases the radio. Only one instance of the
`radio-link` job can run at a time, matching the normal SolarOS job registry.

The Link queues have four entries each and use PSRAM when available. They are
created only when a Link starts, so the compiled service has no idle queue
allocation. Queue overflow increments the drop counter rather than growing
without bound.

With `chat=on`, outgoing broadcast text becomes `sent` after the radio accepts
the frame. Direct text remains `sending` until the matching Link
acknowledgement arrives and becomes `failed` after ten seconds without one.
There is no automatic retry. Text that exceeds the active Link payload MTU
fails with the exact byte limit. Link contacts remain discovered until
explicitly trusted, and Link messages carry no encrypted or transport-secured
security flag.

## Version 1 Frame

All multi-byte values use network byte order:

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 1 | Version in the high nibble, flags in the low nibble |
| 1 | 1 | Message type |
| 2 | 2 | Sequence number |
| 4 | 4 | Source ID |
| 8 | 4 | Destination ID |
| 12 | variable | Payload |
| final 2 | 2 | CRC-16/CCITT-FALSE over header and payload |

Message types are `1` text, `2` binary, and `3` acknowledgement. Unicast text
and binary frames set the acknowledgement-requested flag. An acknowledgement
has no payload, swaps source/destination, and echoes the acknowledged sequence
number. Broadcast frames never request acknowledgements, avoiding an ACK storm.

The receiver remembers the 12 most recent source/sequence/type tuples. A
duplicate is not delivered or copied to the inbox, but is acknowledged again
when requested so a sender can recover from a lost ACK. Up to eight outstanding
unicast sequence/destination pairs are tracked for status; version one does not
automatically retransmit an unacknowledged frame.

The protocol CRC is present even when the transport also supplies a hardware
CRC. This keeps integrity checking consistent across packet radio, serial,
infrared, or future transports.

## Serial byte bridge

The existing `bridge` job can connect one bidirectional byte-stream port to an
active Link:

```text
job start radio-link link0 radio0 lora-eu868
job start bridge uart0 link0 broadcast
```

Serial input is sent as binary Link messages capped to the active payload MTU.
Received text and binary payloads are written back to the serial stream without
separators. Replace `broadcast` with a decimal or `0x` destination ID for
acknowledged unicast. The bridge claims the serial port and consumes the Link
receive queue until stopped.

This is a bounded, best-effort stream adapter rather than Link fragmentation or
flow control. Packet radio can be much slower than a serial producer, so
sustained input can overrun the serial driver or the four-entry Link queue.

## Quick reference

Start a packet-radio link with `job start radio-link link0 radio0
lora-eu868 [inbox=off|on] [chat=off|on]`. Use `link status link0`, `link send
link0 broadcast "text"`, and `link receive link0`. Use `chat=on` for unified
Link broadcast/direct conversations and discovered Contacts. Unicast
destination IDs request acknowledgements. Version one provides bounded
text/binary packet delivery, protocol CRC, and duplicate suppression, but no
routing, fragmentation, encryption, mesh forwarding, or automatic
retransmission.
