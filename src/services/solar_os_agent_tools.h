#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "solar_os_agent_provider.h"

#define SOLAR_OS_AGENT_TOOL_REGISTRY_MAX 8U

typedef enum {
    SOLAR_OS_AGENT_TOOL_RISK_READ_ONLY = 0,
    SOLAR_OS_AGENT_TOOL_RISK_SENSITIVE_READ,
    SOLAR_OS_AGENT_TOOL_RISK_MUTATING,
    SOLAR_OS_AGENT_TOOL_RISK_DISRUPTIVE,
} solar_os_agent_tool_risk_t;

typedef struct {
    solar_os_agent_tool_descriptor_t provider;
    const char *domain;
    const char *output_schema_json;
    const char *required_capability;
    solar_os_agent_tool_risk_t risk;
    bool available;
} solar_os_agent_tool_info_t;

size_t solar_os_agent_tools_collect(
    solar_os_agent_tool_descriptor_t *descriptors,
    size_t capacity);
size_t solar_os_agent_tools_count(void);
bool solar_os_agent_tools_get(size_t index, solar_os_agent_tool_info_t *info);
esp_err_t solar_os_agent_tools_execute(const char *name,
                                       const char *arguments,
                                       char *result,
                                       size_t result_len);
const char *solar_os_agent_tool_risk_name(solar_os_agent_tool_risk_t risk);
