# SolarOS Changelog

## 4.x

- **4.5.0** — 2026-08-02 — Added `writer`, a resumable graphical Markdown
  editor with hybrid source reveal: inactive blocks stay formatted while the
  active or selected blocks expose their exact Markdown. Added PSRAM-backed
  UTF-8 editing up to 256 KiB, bounded undo/redo, shared clipboard,
  find/replace, formatting controls, source-aware vertical navigation, and a
  blinking cursor. Saves use verified staged replacement with rollback, while
  recovery snapshots preserve cursor, scroll, and zoom state after interruption.
- **4.4.13** — 2026-08-01 — Added `calc`, a bounded scientific calculator with
  variables, one-argument functions, worksheet save/load, and a Desmos-style
  expression-and-plot view on graphical displays. Its expression editor uses a
  white background and a thin active-row outline to keep small text legible.
  The same app provides an interactive text REPL and `calc -e` one-shot
  evaluation on UART, USB CDC, Telnet, and other text-only shells.
- **4.4.12** — 2026-08-01 — Added a two-pane `hexedit` application with
  synchronized hexadecimal and ASCII editing. Extended `com` with optional
  hardware UART autobaud detection and an offset/hex/ASCII receive view while
  preserving its normal text-terminal mode, named-bus leasing, and display and
  port-shell support.
- **4.4.11** — 2026-08-01 — Added physical connector-layout views to the `io`
  application and shell commands, including compact board connectors and
  paged DevKit headers. Fixed SPI teardown for transient devices. Retained the
  working TinyUSB HID implementation as a dormant package excluded from
  standard builds because of its internal-SRAM cost, and restored the original
  USB Serial/JTAG FIFO sizing.
- **4.4.10** — 2026-07-31 — Fixed Wi-Fi and BLE radio recovery after light
  sleep, made long KEY presses replace the remembered BLE keyboard, added a
  NeoPixel expansion driver with Python and Lua bindings, added edge-triggered
  logic-analyzer capture, and let live plots request faster scheduler ticks.
- **4.4.9** — 2026-07-31 — Added an optional SolarOS Link provider for unified Chat. `radio-link
  ... chat=on` creates a broadcast conversation, discovers 32-bit Link source
  IDs as Contacts, projects incoming text without consuming the Link receive
  queue, sends packet-sized direct and broadcast text, and maps unicast
  acknowledgements and timeouts to delivery state.

- **4.4.8** — 2026-07-30 — The Agent app now exits with `Esc` as well as the
  app-exit key. SSH and SCP behave like normal command-line tools: they keep the
  current terminal contents, print their result inline, and return directly to
  the shell prompt. Playground now stores its catalog and applications in the
  visible `/playground` directory, and `playground delete` removes that
  directory, cleans up the legacy hidden location, and clears the loaded
  catalog while retaining source and storage preferences.
- **4.4.7** — 2026-07-30 — Applications and jobs can request scheduler ticks
  faster than the former fixed 25 ms cadence. Python and Lua scripts can use
  `solaros.tick_interval([ms])`, including `solaros.tick_interval(5)` for a
  5 ms cadence and `solaros.tick_interval(0)` to restore the default. The
  scheduler uses the fastest active request while preserving cooperative
  execution.
- **4.4.6** — 2026-07-30 — Fixed repeat Playground catalog refreshes on FAT
  filesystems, added a persistent flash/SD target shared by the catalog and
  default application installations, and made SD the default when available
  and no setting exists.
  The catalog tree and application details now use `i` to install or update and
  `u` to uninstall. Added `nvs status` for partition, entry, and namespace
  diagnostics, and `nvs clear` to erase all NVS-backed settings and reboot.
- **4.4.5** — 2026-07-30 — Added provider-neutral messaging, PSRAM-backed
  Contacts and Credentials services, bounded Conversations/Messages storage,
  offline provider outboxes, Inbox projection, live `messages` shell controls,
  safe Python/Lua APIs, and an offline-capable Chat client with live provider
  and contact identity handling. Added the pinned MeshCore companion provider
  with Ed25519/ECDH direct messaging, shared-key groups, ACK retries, discovery
  and trust enforcement, an RFM95-compatible adapter, an EU868 profile, a
  background job, and shell controls. Hardened MeshCore lifecycle transitions,
  contact-cache updates and blocking, credential persistence, memory use, and
  connected-device regression coverage.
- **4.4.4** — 2026-07-30 — Added transport-independent SolarOS Link v1 framing
  for text, binary, and acknowledgement messages with stable device IDs,
  broadcast delivery, CRC checks, bounded queues, duplicate suppression, and
  ACK tracking. The `radio-link` job and `link` shell commands provide managed
  packet-radio transport. The `bridge` job can now connect a bidirectional
  byte-stream port to an active Link for broadcast or acknowledged unicast
  traffic.
- **4.4.3** — 2026-07-30 — Added the RFM95W expansion driver with LoRa, FSK,
  GFSK, MSK, GMSK, and OOK modes, including packet and FIFO-stream operation,
  signal reporting, CRC, sync words, addressing, and Gaussian shaping. SPI
  devices now claim only their selected chip-select GPIO, leaving other
  candidates available for roles such as reset or data/command. Added atomic
  complete-radio profiles with built-in EU868 settings and eight persistent
  user profiles.
- **4.4.2** — 2026-07-29 — Reduced SD-card boot time by trusting the signed
  active manual catalog instead of reopening and hashing every previously
  verified Markdown page. Manual downloads and updates still verify the
  signature, archive, page sizes, and page hashes before activation.
- **4.4.1** — 2026-07-29 — Added resumable application sessions to UART, USB
  CDC, and network shells. `Ctrl+Z` suspends a resumable port application,
  `fg [id]` restores it on its owning terminal, and the global session list and
  close commands include port-owned applications. Child launches now suspend
  and return to Files or Playground correctly. Cross-shell creation, focus, and
  closure of display sessions remain unchanged. Port-session metadata and
  control queues prefer PSRAM and add no task stack per suspended app. SolarOS
  refuses to close the final interactive shell, and closing the current port
  shell no longer redraws a stale prompt before disconnecting. Closing a
  detached display application now restores a valid base context before its
  parent resumes, preventing a freed terminal from being drawn and rebooting
  the device. Playground now exits its top-level TUI with `Esc` and provides
  shell-usable `search`, `install`, and `run` subcommands with matching
  autocomplete and manual coverage. Install and run autocomplete streams
  installed application IDs directly from the loaded catalog without a second
  RAM cache. `playground run` now resolves the entry in the shell and launches
  Python or Lua directly instead of creating an intermediate Playground
  session. Opening the Playground TUI no longer refreshes an unavailable
  catalog automatically, so merely opening it never requires Wi-Fi. Successful
  refreshes now persist the catalog to flash; TUI startup and the new
  `playground reload` command load that local copy without network access.
- **4.4.0** — 2026-07-29 — Added Playground, a native foldable catalog browser
  for community Python and Lua applications. Users can search one configurable
  repository, inspect compatibility and installation state, verify and install
  hashed application archives to flash or SD, update or remove them, and launch
  the selected runtime. Added source selection in NVS, staged replacement,
  shell autocomplete, a package-aware manual entry, and the initial
  `solar_os_playground` repository contract with deterministic catalog/archive
  generation and Python/Lua examples. GitHub repository source URLs are
  normalized to their generated catalog, including values saved by 4.4.0.
- **4.3.20** — 2026-07-29 — Added an ASCII character-set mode for port-shell
  TUIs used through legacy serial terminals. `setterm charset ascii` and
  `session create shell ... --charset ascii` now replace Unicode box drawing,
  blocks, arrows, and punctuation with readable ASCII while leaving display
  sessions and UTF-8 terminals unchanged. Added matching autocomplete and
  Python/Lua session options.
- **4.3.19** — 2026-07-29 — Completed shell argument autocomplete across
  applications, commands, and job startup: app flags and values, filesystem
  operands, GPIO/logic inputs, manual references, and live conversation,
  inbox, expansion-device, port, display, stream, and job identifiers are now
  suggested where applicable. Added the previously omitted `gpio release`
  subcommand.
- **4.3.18** — 2026-07-29 — Extended `setterm palette normal|inverted` to the
  shared graphics palette so graphic apps reverse black, white, and dithered
  shades without framebuffer rewriting or controller inversion. Headless port
  shells can save the palette before an expansion-display session exists.
- **4.3.17** — 2026-07-29 — Added a persistent `setterm palette
  normal|inverted` setting that reverses terminal black and white independently
  of display-controller inversion modes.
- **4.3.16** — 2026-07-29 — Added `exit` to close the current UART, USB CDC, or
  telnet shell cleanly while leaving the built-in display shell active.
- **4.3.15** — 2026-07-29 — Added target-addressed foreground app creation
  with `session create <app> <display-target> [args...]`. Local BLE keyboard,
  board-button, joystick, and D-pad input now follows an explicit display focus
  instead of the globally foreground session; `session focus [display-target]`
  inspects or changes that assignment, and `Alt+Tab` cycles only sessions on
  the focused display. Port shells retain their own input while launching apps
  or display shells remotely. Suspended sessions no longer receive ticks or
  leak their terminal context into another app on the same display, preventing
  an auto-started display app and its backing shell from alternately rendering.
  Remote display-session creation and closure now run on the main scheduler
  rather than a telnet or port-shell task, preventing concurrent framebuffer
  and panel access from corrupting the built-in display during startup or
  teardown.
- **4.3.14** — 2026-07-29 — Gave the ESP32-S3-DevKitC-1-N16R8 two 6 MiB OTA
  slots and a nearly 4 MiB internal filesystem, replacing the 64 KiB shared
  layout so durable agent conversations and local files have practical space
  without an SD card. Fixed streamed agent responses in VT100 port shells by
  emitting CRLF newlines instead of staircase-producing bare line feeds. Made
  local-model tool use more reliable by exposing prompt-relevant operations,
  forbidding unverified success claims in the agent instructions, and feeding
  recoverable tool failures back to the model instead of aborting the request.
  Preserved prior tool results across stateless Chat Completions turns, stopped
  duplicate read-only tool loops, and accepted full-size streamed tool
  arguments from local OpenAI-compatible providers. Agent storage tools now
  resolve relative paths from the invoking shell directory, and tool failures
  show their concrete error name in the TUI. Clarified the exact
  `solaros_reference` argument shape for local models and stop advertising
  tools after the first repeated read-only call. Known policy-allowed tools
  requested outside the active schema set now activate for a schema-backed
  retry instead of failing the workflow. Added exact-path `storage_stat`
  inspection so file-existence questions do not misuse content search, and
  retry one empty stateless-provider turn instead of silently ending it.
  Restricted on-demand activation to declared workflow dependencies and made
  the distinction between exact-path metadata and content search explicit;
  path-like prompts now select `storage_stat` deterministically before fuzzy
  tool matching.
  (`bf4f600`, `6ed07f5`, `093ffac`)
- **4.3.13** — 2026-07-28 — Added bounded durable native-agent conversations
  in three flash or eight SD slots, with atomic checked records and explicit
  new/list/resume/delete operations,
  restored TUI transcripts, Responses continuation IDs, and bounded local
  history for Chat Completions. Added read-only native-agent workload
  inspection with centralized admission results, memory headroom, generations,
  failure reasons, and current resource claims. (`8de1c54`, `0374c24`,
  `8e3bdca`)
- **4.3.12** — 2026-07-28 — Raised the native agent's default tool budget to
  16 and configurable maximum to 32 calls, made the reserved final provider
  turn tool-free, and added per-request budget usage to `agent status`.
  (`281f45f`)
- **4.3.11** — 2026-07-28 — Made `doc/manual/` the comprehensive source shared
  by GitHub, the website, device help, and the native agent. Added the foldable
  `help` browser, `commands` and package-aware `man`, display-aware
  `reader`/`less` routing, and fail-safe exact-version updates through one
  catalog-authenticated manual archive with embedded fallback. (`10c39d2`)
- **4.3.10** — 2026-07-26 — Added the native resumable agent with OpenAI
  Responses reasoning, configurable tool policy and limits, progressive
  SolarOS-grounded tools, and safe script/storage development operations.
  Improved foreground switching and status rendering, and reduced Wi-Fi SRAM
  use. (`e7866d7`)
- **4.3.9** — 2026-07-26 — Moved device identity to NVS, added identity controls, and advertised the configured hostname over Wi-Fi. (`1acfe134`)
- **4.3.8** — 2026-07-26 — Improved job diagnostics with bold running rows, worker-stack requirements, and clearer waiting-versus-failed reporting; restored ODROID-GO IRAM headroom. (`0c73f0cd`, `f239ca6b`)
- **4.3.7** — 2026-07-25 — Added target-addressed routing to `displayd`. (`b9514e17`)
- **4.3.6** — 2026-07-24 — Bound display sessions to their parent framebuffers. (`9af40321`)
- **4.3.5** — 2026-07-24 — Added the `telnetd` background service. (`cc5f91ae`)
- **4.3.4** — 2026-07-24 — Marked the release that reserved internal executor stacks and queued background jobs under SRAM pressure. (`381277c7`)
- **4.3.3** — 2026-07-23 — Adopted a PSRAM-first allocation policy. (`ddd7865c`)
- **4.3.2** — 2026-07-22 — Added a reusable HTTP client service. (`d39e6c7b`)
- **4.3.1** — 2026-07-22 — Added the background chat client synchronizer. (`fb03fb7b`)
- **4.2.21** — 2026-07-21 — Cleaned up board metadata. (`ec4e9ee3`)
- **4.2.20** — 2026-07-20 — Completed adoption of the memory policy. (`cf83b313`)
- **4.2.14** — 2026-07-20 — Added cooperative-scheduling timing controls. (`17a6091d`)
- **4.2.13** — 2026-07-20 — Added paired creation and deletion helpers for external stacks. (`69bdb5bc`)
- **4.2.11** — 2026-07-20 — Added I/O autostart support that appends commands to the startup script. (`2bb7c5ca`)
- **4.2.10** — 2026-07-20 — Expanded expansion-command help and autocomplete. (`3f2afd3b`)
- **4.2.9** — 2026-07-20 — Added concurrency protection for buses. (`59de5c70`)
- **4.2.8** — 2026-07-20 — Added concurrency protection for services. (`3e6a860f`)
- **4.2.7** — 2026-07-19 — Added the I/O TUI. (`7e91364e`)
- **4.2.6** — 2026-07-19 — Kept UART descriptors registered and made them non-removable. (`f4970d73`)
- **4.2.5** — 2026-07-19 — Added named UART support. (`7d5dc797`)
- **4.2.4** — 2026-07-19 — Added a named OneWire backend and discovery. (`eab3a212`)
- **4.2.3** — 2026-07-19 — Consolidated board buses into one canonical `SOLAR_OS_BOARD_BUSES` table. (`9050aa84`)
- **4.2.2** — 2026-07-18 — Added board pin policies and atomic claim bundles. (`8c897a0c`)
- **4.2.1** — 2026-07-18 — Allowed the editor to run without PSRAM. (`be956343`)
- **4.2.0** — 2026-07-18 — Added explicit memory-allocation classes and guarded PSRAM fallback. (`398c8e36`)
- **4.1.3** — 2026-07-17 — Added the Inbox app. (`d0a1580a`)
- **4.1.2** — 2026-07-17 — Fixed startup handling and omitted icons on compact displays. (`4937eb17`)
- **4.1.1** — 2026-07-17 — Added SSD1306 and SH1106 OLED expansion-display support. (`2b8eaa28`)
- **4.1.0** — 2026-07-17 — Made internal flash the default storage when no SD card is present. (`7bc00877`)
- **4.0.0** — 2026-07-17 — Added CrowPanel 4.2-inch e-paper board support. (`f067859a`)

## 3.x

- **3.9.4** — 2026-07-16 — Changed the default OTA endpoint. (`73aed466`)
- **3.9.3** — 2026-07-14 — Applied capability gates to the Python and Lua APIs. (`46f0a8c2`)
- **3.9.2** — 2026-07-14 — Added OneWire bindings for Python and Lua. (`1697688b`)
- **3.9.1** — 2026-07-14 — Added OneWire support. (`821ab479`)
- **3.9.0** — 2026-07-14 — Added the SUMP job and Logic app. (`4a5ea682`)
- **3.8.4** — 2026-07-07 — Increased the maximum accepted OTA index size. (`921ecc54`)
- **3.8.3** — 2026-07-07 — Added wildcard support to SCP. (`02cc9c94`)
- **3.8.2** — 2026-07-07 — Added SD-card unmount support. (`859527d4`)
- **3.8.1** — 2026-07-07 — Published a documentation-only maintenance release. (`80aba5b2`)
- **3.8.0** — 2026-07-04 — Added SIMD engine API plumbing. (`5e4ac543`)
- **3.7.0** — 2026-07-03 — Split display targets. (`0b0f631a`)
- **3.6.1** — 2026-07-03 — Added terminal-capability handling to port shells. (`ca9d0dba`)
- **3.6.0** — 2026-07-02 — Cleaned up root-path and default-storage handling. (`4331013c`)
- **3.5.0** — 2026-07-02 — Added expansion capabilities and the expansion service. (`36e3356c`)
- **3.4.0** — 2026-06-30 — Added D-pad support. (`f79f8d30`)
- **3.3.1** — 2026-07-01 — Fixed I2S DMA allocation. (`f806023e`)
- **3.3.0** — 2026-06-29 — Added RAMFS. (`51875f03`)
- **3.2.0** — 2026-06-29 — Restored child-session support. (`78215a4b`)
- **3.1.0** — 2026-06-29 — Added the Telnet foreground app. (`b21ad0c0`)
- **3.0.0** — 2026-06-29 — Added TUI caching. (`9a67767a`)

## 2.x

- **2.9.8** — 2026-06-29 — Added package-tree output. (`8641b601`)
- **2.9.7** — 2026-06-28 — Added note categories. (`4eae673e`)
- **2.9.6** — 2026-06-28 — Changed SCP's default-target behavior. (`097579e2`)
- **2.9.5** — 2026-06-28 — Added filename tab completion. (`b459c7ff`)
- **2.9.4** — 2026-06-28 — Closed SSH session requests when setup fails. (`93989048`)
- **2.9.3** — 2026-06-28 — Exposed the graphics capability to Lua and Python. (`231382b1`)
- **2.9.2** — 2026-06-27 — Adjusted OTA task stack sizing. (`bd616761`)
- **2.9.1** — 2026-06-27 — Reduced OTA heap pressure and adjusted the shell stack. (`288aed73`)
- **2.9.0** — 2026-06-27 — Added cooperative foreground sessions. (`f5d7cac4`)
- **2.8.0** — 2026-06-27 — Added power management. (`db30d271`)
- **2.7.3** — 2026-06-27 — Shut down the radio before sleep. (`2b822b57`)
- **2.7.2** — 2026-06-27 — Added shell terminal probes. (`e1cd39e5`)
- **2.7.1** — 2026-06-27 — Allowed SSH to run without storage. (`9cd7509a`)
- **2.7.0** — 2026-06-27 — Added the JSON service. (`633ee69e`)
- **2.6.1** — 2026-06-27 — Added OTA schemas. (`71f3b6d1`)
- **2.6.0** — 2026-06-27 — Added the board-capability layer. (`0c04bf46`)
- **2.5.1** — 2026-06-27 — Added the item editor to Notes. (`8646a2a3`)
- **2.5.0** — 2026-06-26 — Added the Files commander app. (`a477d815`)
- **2.4.2** — 2026-06-26 — Added the splash screen. (`d90f79cb`)
- **2.4.1** — 2026-06-26 — Added the document asset provider. (`2fa610ec`)
- **2.4.0** — 2026-06-26 — Added ZIP support. (`1460d9ac`)
- **2.3.0** — 2026-06-25 — Added the Xfer app. (`46aea50f`)
- **2.2.1** — 2026-06-25 — Added the Notes app. (`47ef9b7d`)
- **2.2.0** — 2026-06-24 — Added the Plot app. (`8fcf2c1e`)
- **2.1.1** — 2026-06-24 — Returned to single BLE-keyboard support. (`5bea632a`)
- **2.1.0** — 2026-06-24 — Added the Sheet app. (`986e9ab2`)
- **2.0.1** — 2026-06-24 — Added multistream data acquisition. (`5095c8e2`)
- **2.0.0** — 2026-06-24 — Added the stream service and data-acquisition job. (`218fd988`)

## 1.x

- **1.9.8** — 2026-06-24 — Added deep-shell support. (`fa18b979`)
- **1.9.7** — 2026-06-24 — Added remembered Wi-Fi networks. (`685456cf`)
- **1.9.6** — 2026-06-24 — Added monochrome xterm handling for SSH. (`87b88bf2`)
- **1.9.5** — 2026-06-24 — Added the battery monitor. (`2e818541`)
- **1.9.4** — 2026-06-24 — Fixed UART status reporting. (`37385638`)
- **1.9.3** — 2026-06-24 — Integrated Lua and BLE support. (`840d2916`)
- **1.9.0** — 2026-06-23 — Added the Lua REPL. (`1980162f`)
- **1.8.0** — 2026-06-23 — Added the power service and shell controls. (`a35fd558`)
- **1.7.5** — 2026-06-23 — Made OTA updates flavor-aware. (`a817bc5d`)
- **1.7.0** — 2026-06-23 — Added build packaging and firmware flavors. (`b068e725`)
- **1.6.0** — 2026-06-23 — Added MP3 playback. (`4c933216`)
- **1.5.0** — 2026-06-23 — Added the web browser. (`a2878970`)
- **1.4.1** — 2026-06-22 — Optimized the chat app and service. (`dc0d4c7f`)
- **1.4.0** — 2026-06-22 — Added the initial work-in-progress chat gateway. (`795c39cb`)
- **1.3.1** — 2026-06-22 — Fixed battery charge-state reporting. (`72582901`)
- **1.3.0** — 2026-06-22 — Added the MQTT service and shell/Python integration. (`52076d78`)
- **1.2.1** — 2026-06-22 — Added global audio-level control. (`e87a53b8`)
- **1.2.0** — 2026-06-21 — Added the memory service and integrated it with the shell, Python runtime, and SLIP job. (`d779a790`)
- **1.1.0** — 2026-06-21 — Added the SLIP background job. (`cdb85cb6`)
- **1.0.0** — 2026-06-21 — Established the initial SolarOS firmware, services, shell, apps, jobs, MicroPython runtime, and Waveshare ESP32-S3 RLCD board support. (`7ab94ab9`)
