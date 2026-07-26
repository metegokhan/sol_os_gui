# SolarOS Changelog

## Unreleased

- Added the first native agent slice: a provider-neutral service, bounded
  OpenAI-compatible streaming adapter, NVS-backed configuration, cancellable
  `agent` app, and read-only `system_status` tool. Shell-only subcommands run
  inline so their output remains visible when the prompt returns. Official
  OpenAI Chat Completions requests disable reasoning effort when function tools
  are present, as required by that endpoint. Added an endpoint-selected
  Responses API path with configurable reasoning effort, typed streaming
  events, and reasoning-preserving tool continuations. Added a declarative
  typed-tool registry with schemas, risk and availability metadata, validated
  JSON results, and read-only system status, storage listing, and job listing
  tools. Added a reusable Python/Lua source-and-file runner with bounded
  captured output, structured completion, VM-level cancellation and deadlines,
  interpreter ownership guards, and a manual `agent script` validation path.
  Added NVS-backed `off`, `readonly`, `confirm`, and `all` tool policy with
  defense-in-depth executor checks, bounded local confirmation, denial
  continuations and tool counters; policy-gated Python and Lua source execution
  is now available to the model when the corresponding runtime is installed.
  Bare `agent` now opens a resumable foreground conversation, Responses sessions
  retain context while the app remains open, and one-shot responses wait for
  explicit exit instead of immediately returning to the display shell. Expanded
  the bounded tool chain to five executions plus a final response and added
  confirmed, size-limited storage reads and writes while excluding SSH identity
  paths.

## 4.x

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
