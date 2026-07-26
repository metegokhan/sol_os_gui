# Native Agent Service

`service.agent` is the native control plane for connecting SolarOS to remote
language models. `app.agent` is currently a one-request text frontend for
display and port shells.

The first provider supports both the OpenAI Responses API and compatible Chat
Completions endpoints. Configure the complete endpoint URL rather than a
provider base URL. Responses is recommended for OpenAI reasoning models:

```text
agent config endpoint https://api.openai.com/v1/responses
agent config model gpt-model
agent config key api-key
agent config reasoning medium
agent status
```

The endpoint path selects the wire protocol: URLs ending in `/responses` use
typed Responses events, while other URLs use Chat Completions. Reasoning effort
may be `none`, `minimal`, `low`, `medium`, `high`, `xhigh`, or `max`; support
still depends on the selected model. The default is `medium`.

Responses tool continuations use `previous_response_id`, so reasoning context
is retained without copying reasoning tokens through the device. Responses are
stored by the provider for that request chain. Instructions are sent again on
the continuation because Responses does not carry previous instructions
forward automatically.

For the official OpenAI Chat Completions endpoint, the adapter instead sends
`reasoning_effort: none` so function tools remain compatible with that API.
Other Chat-compatible endpoints do not receive this provider-specific field.

Configuration is stored in the `agent` NVS namespace. The API key is used as a
Bearer token and is never returned by status. Use `agent config key clear` for
a local or self-hosted endpoint without authentication, or `agent forget` to
erase the endpoint, model, and key together.

Ask a question with:

```text
agent ask Describe the current device status.
```

The adapter requests server-sent streaming events and emits provider-neutral
events for text deltas, usage, tool calls, errors, and completion. Cancellation
uses the shared HTTP client's cross-task cancellation path. TLS certificate
validation uses the firmware certificate bundle.

## Initial tool

The initial registry contains one read-only tool:

- `system_status`: board ID, SolarOS version, uptime, free and largest internal
  RAM blocks, and free PSRAM.

The service permits one tool execution and one continuation turn. Unsupported
tools, multiple simultaneous tool calls, malformed arguments, or another tool
request in the continuation turn fail the request instead of extending the
loop.

## Resource bounds

- Foreground worker stack: 16 KiB internal RAM.
- Stream-line buffer: 24 KiB, PSRAM preferred. Responses completion events
  contain the assembled output as one event.
- Request body: 8 KiB, PSRAM preferred.
- Prompt: 1023 bytes.
- Tool arguments: 511 bytes.
- Tool result: 767 bytes.
- Model output: 16 KiB per provider turn.
- Request deadline: 90 seconds.
- Per-I/O timeout: 15 seconds.

`agent status` records internal free memory, the lowest sample observed during
HTTP streaming, largest internal blocks, and PSRAM before and at request
completion. The completion sample still includes the foreground worker stack;
run `mem policy` after the app returns to confirm that the task stack was
reclaimed.

The package requires Wi-Fi and PSRAM but does not require Python or Lua. Script
execution, persistent conversations, more tools, additional providers, and a
resumable conversation UI are later phases.
