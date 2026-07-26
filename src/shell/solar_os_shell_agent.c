#include "solar_os_shell_commands.h"

#include <inttypes.h>
#include <string.h>

#include "solar_os_agent.h"
#include "solar_os_agent_app.h"
#include "solar_os_agent_tools.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static void agent_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_write_bold(io, "agent");
    solar_os_shell_io_newline(io);
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  agent");
    solar_os_shell_io_writeln(io, "  agent help");
    solar_os_shell_io_writeln(io, "  agent status");
    solar_os_shell_io_writeln(io, "  agent tools");
    solar_os_shell_io_writeln(io, "  agent config endpoint URL");
    solar_os_shell_io_writeln(io, "  agent config model MODEL");
    solar_os_shell_io_writeln(io, "  agent config key KEY|clear");
    solar_os_shell_io_writeln(
        io,
        "  agent config reasoning none|minimal|low|medium|high|xhigh|max");
    solar_os_shell_io_writeln(
        io,
        "  agent config tools off|readonly|confirm|all");
    solar_os_shell_io_writeln(io, "  agent config max-tools 1..12");
    solar_os_shell_io_writeln(io, "  agent forget");
    solar_os_shell_io_writeln(io, "  agent ask PROMPT...");
    solar_os_shell_io_writeln(
        io,
        "  agent script python|lua (-c SOURCE | FILE) [ARGS...]");
}

static void agent_tools(solar_os_shell_io_t *io)
{
    solar_os_agent_status_t status = {0};
    (void)solar_os_agent_get_status(&status);
    solar_os_shell_io_printf(
        io,
        "Policy: %s\n",
        solar_os_agent_tool_policy_name(status.tool_policy));
    solar_os_shell_io_writeln(
        io,
        "Name               Domain    Risk            Available  Policy   Requirement");
    const size_t count = solar_os_agent_tools_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_agent_tool_info_t tool;
        if (!solar_os_agent_tools_get(i, &tool)) {
            continue;
        }
        const solar_os_agent_tool_policy_decision_t decision =
            solar_os_agent_tools_policy_decision(status.tool_policy, tool.risk);
        const char *policy = decision == SOLAR_OS_AGENT_TOOL_POLICY_ALLOW ?
            "allow" :
            (decision == SOLAR_OS_AGENT_TOOL_POLICY_CONFIRM_ONCE ?
                "confirm" : "deny");
        solar_os_shell_io_printf(
            io,
            "%-18s %-9s %-15s %-10s %-8s %s\n",
            tool.provider.name,
            tool.domain,
            solar_os_agent_tool_risk_name(tool.risk),
            tool.available ? "yes" : "no",
            policy,
            tool.required_capability != NULL ?
                tool.required_capability : "-");
    }
}

static void agent_status(solar_os_shell_io_t *io)
{
    solar_os_agent_status_t status;
    if (solar_os_agent_get_status(&status) != ESP_OK) {
        solar_os_shell_io_writeln(io, "agent: status unavailable");
        return;
    }

    solar_os_shell_io_printf(io,
                             "Provider: %s\n"
                             "Endpoint: %s\n"
                             "Model: %s\n"
                             "API key: %s\n"
                             "Reasoning (Responses): %s\n"
                             "Tool policy: %s\n"
                             "Max tools/request: %u\n"
                             "State: %s\n",
                             status.provider,
                             status.endpoint[0] != '\0' ?
                                 status.endpoint : "not configured",
                             status.model[0] != '\0' ?
                                 status.model : "not configured",
                             status.api_key_set ? "set" : "not set",
                             status.reasoning_effort,
                             solar_os_agent_tool_policy_name(status.tool_policy),
                             (unsigned int)status.max_tools,
                             status.running ? "running" : "idle");
    solar_os_shell_io_printf(io,
                             "Requests: %" PRIu32 ", failures: %" PRIu32 "\n"
                             "Tools: %" PRIu32 " executed, %" PRIu32
                             " denied, %" PRIu32 " failed\n",
                             status.request_count,
                             status.failure_count,
                             status.tool_executed_count,
                             status.tool_denied_count,
                             status.tool_failed_count);
    if (status.request_count == 0) {
        return;
    }

    solar_os_shell_io_printf(
        io,
        "Last: %s, HTTP %d, %" PRIu32 " ms, %" PRIu32 " bytes\n",
        esp_err_to_name(status.last_error),
        status.last_http_status,
        status.last_duration_ms,
        status.last_bytes_received);
    solar_os_shell_io_printf(
        io,
        "Internal: before %" PRIu32 ", low %" PRIu32
        ", request-end %" PRIu32 " bytes\n",
        status.last_internal_before,
        status.last_internal_low,
        status.last_internal_after);
    solar_os_shell_io_printf(
        io,
        "Largest internal: before %" PRIu32
        ", request-end %" PRIu32 " bytes\n",
        status.last_internal_largest_before,
        status.last_internal_largest_after);
    solar_os_shell_io_printf(
        io,
        "PSRAM: before %" PRIu32 ", request-end %" PRIu32 " bytes\n",
        status.last_psram_before,
        status.last_psram_after);
}

static void agent_configure(solar_os_shell_io_t *io, int argc, char **argv)
{
    if (argc != 4) {
        agent_usage(io);
        return;
    }

    const char *field = argv[2];
    const char *value = argv[3];
    esp_err_t err;
    if (strcmp(field, "endpoint") == 0) {
        err = solar_os_agent_set_endpoint(value);
    } else if (strcmp(field, "model") == 0) {
        err = solar_os_agent_set_model(value);
    } else if (strcmp(field, "key") == 0) {
        err = solar_os_agent_set_api_key(strcmp(value, "clear") == 0 ? "" : value);
    } else if (strcmp(field, "reasoning") == 0) {
        err = solar_os_agent_set_reasoning_effort(value);
    } else if (strcmp(field, "tools") == 0) {
        solar_os_agent_tool_policy_t policy;
        err = solar_os_agent_parse_tool_policy(value, &policy);
        if (err == ESP_OK) {
            err = solar_os_agent_set_tool_policy(policy);
        }
    } else if (strcmp(field, "max-tools") == 0) {
        uint8_t max_tools = 0;
        if (!solar_os_shell_parse_u8(value, &max_tools)) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            err = solar_os_agent_set_max_tools(max_tools);
        }
    } else {
        agent_usage(io);
        return;
    }

    if (err == ESP_OK) {
        solar_os_shell_io_printf(io,
                                 "agent: %s %s\n",
                                 field,
                                 strcmp(field, "key") == 0 ?
                                     (strcmp(value, "clear") == 0 ? "cleared" : "set") :
                                     "updated");
    } else {
        solar_os_shell_io_printf(io,
                                 "agent: invalid %s: %s\n",
                                 field,
                                 esp_err_to_name(err));
    }
}

void solar_os_shell_cmd_agent(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }

    if (argc == 1) {
        const esp_err_t err =
            solar_os_context_request_launch(ctx, &solar_os_agent_app, argc, argv);
        if (err == ESP_OK) {
            solar_os_shell_session_prepare_foreground_launch(ctx, false);
        } else {
            solar_os_shell_io_printf(io,
                                     "agent: launch failed: %s\n",
                                     esp_err_to_name(err));
        }
        return;
    }
    if (argc == 2 && strcmp(argv[1], "help") == 0) {
        agent_usage(io);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        agent_status(io);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "tools") == 0) {
        agent_tools(io);
        return;
    }
    if (strcmp(argv[1], "config") == 0) {
        agent_configure(io, argc, argv);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "forget") == 0) {
        const esp_err_t err = solar_os_agent_forget();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(io, "agent: configuration erased");
        } else {
            solar_os_shell_io_printf(io,
                                     "agent: erase failed: %s\n",
                                     esp_err_to_name(err));
        }
        return;
    }
    if (strcmp(argv[1], "ask") == 0 && argc >= 3) {
        const esp_err_t err =
            solar_os_context_request_launch(ctx, &solar_os_agent_app, argc, argv);
        if (err == ESP_OK) {
            solar_os_shell_session_prepare_foreground_launch(ctx, false);
        } else {
            solar_os_shell_io_printf(io,
                                     "agent: launch failed: %s\n",
                                     esp_err_to_name(err));
        }
        return;
    }
    if (strcmp(argv[1], "script") == 0 && argc >= 4) {
        const esp_err_t err =
            solar_os_context_request_launch(ctx, &solar_os_agent_app, argc, argv);
        if (err == ESP_OK) {
            solar_os_shell_session_prepare_foreground_launch(ctx, false);
        } else {
            solar_os_shell_io_printf(io,
                                     "agent: launch failed: %s\n",
                                     esp_err_to_name(err));
        }
        return;
    }
    agent_usage(io);
}
