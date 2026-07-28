+++
id = "agent"
title = "Native SolarOS agent"
section = "app"
summary = "Configure and use the resumable LLM agent and its typed tools"
aliases = ["llm"]
keywords = "agent llm model openai responses reasoning tools policy prompt chat"
packages_any = ["app_agent"]
+++
# Native SolarOS agent

The `agent` application is a foreground chat that can inspect the current
device, consult this manual, edit files, and run installed Python or Lua
runtimes. It exposes only tools allowed by the current firmware and tool policy.

## Configure it

The Responses API is recommended for OpenAI reasoning models:

```text
agent config endpoint https://api.openai.com/v1/responses
agent config model gpt-model
agent config key api-key
agent config reasoning medium
agent config tools confirm
agent config max-tools 16
agent status
```

The default `confirm` policy runs read-only tools automatically and asks before
sensitive reads, file changes, or script execution. Use `readonly` when the
agent should only inspect the system.

## Start a conversation

Run `agent` or `agent new`, enter a message, and wait for the response. The
bottom bar shows
the latest input, output, and total token counts. Tool activity stays in the
conversation, while usage does not.

Completed turns are saved. Inspect and resume them explicitly:

```text
agent list
agent resume CONVERSATION-ID
agent delete CONVERSATION-ID
```

`agent resume` restores the transcript. Responses endpoints continue from the
saved provider response ID; Chat Completions endpoints receive a bounded local
message window. Bare `agent` always starts a new conversation.

Use `agent ask PROMPT` for one request. It leaves the answer visible until you
press `Ctrl+]`; one-request answers are not added to the conversation store.

## Troubleshooting

- `agent status` shows the endpoint, model, policy, request failures, and memory
  telemetry without printing the API key. It also reports how many tool calls
  the last request used from its configured budget.
- If a generated script guesses an API or display name, ask the agent to call
  `solaros_reference` and the relevant discovery tool first.
- If a call is denied, the model receives a structured denial and can explain
  the result or choose another approach.

## Quick reference

Use `agent` or `agent new` for a new durable foreground conversation,
`agent list` to find saved IDs, `agent resume ID` to restore one, and
`agent delete ID` to remove one. `agent ask PROMPT` makes an unsaved one-shot
request. Configure endpoint, model, key, reasoning, tool policy, and maximum
tools with `agent config`. `agent tools` reports the installed typed tools and
their policy disposition. The default `confirm` policy runs read-only tools
and requires local approval for sensitive, mutating, or disruptive calls.
Ctrl+] returns to the shell.
