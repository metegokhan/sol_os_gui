#include "solar_os_agent_reference.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define AGENT_REFERENCE_MATCH_MAX 3U
#define AGENT_REFERENCE_TOKEN_MAX 31U

static const char *const AGENT_REFERENCE_GUIDANCE =
    "Mandatory SolarOS coding rules: use only symbols and constants documented "
    "in the returned matches. Never replace color, font, key, GPIO mode, or "
    "other constants with guessed strings or numbers. Never invent device, "
    "display, bus, or GPIO names. Treat optional modules as package-gated. "
    "Follow begin/end and cleanup patterns even on errors. If a needed API is "
    "not documented here, call solaros_reference again before writing code.";

typedef struct {
    const char *topic;
    const char *keywords;
    const char *reference;
} agent_reference_entry_t;

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} agent_reference_output_t;

/*
 * Keep these compact contracts synchronized with doc/solar_os_python.md and
 * doc/solar_os_lua.md. They are deliberately signatures and operational
 * constraints rather than tutorials so a model can retrieve only the relevant
 * SolarOS surface before generating code.
 */
static const agent_reference_entry_t AGENT_REFERENCE_ENTRIES[] = {
    {
        .topic = "overview",
        .keywords = "solaros api modules python lua help reference capabilities",
        .reference =
            "Search again with a module or task name. Topics cover gfx, tui, "
            "storage, identity, jobs, sessions, apps, wifi, mqtt, net, gpio, "
            "adc, pwm, buses, i2c, spi, uart, onewire, expansion, audio, ble, "
            "clipboard, time, battery, sensors, and ssh_keys. Optional modules "
            "exist only when their package is installed.",
    },
    {
        .topic = "python.gfx",
        .keywords =
            "python py gfx graphics display screen draw pixel line rectangle "
            "circle font oled lcd framebuffer present",
        .reference =
            "Python: import solaros; from solaros import gfx. gfx.begin() uses "
            "the current foreground display and raises RuntimeError from a "
            "port/headless shell where there is none. For an attached display, "
            "the agent must call display_list and pass a returned ready name "
            "to gfx.begin(name); scripts can verify names with "
            "solaros.expansion.devices(). An absent name raises "
            "ESP_ERR_NOT_FOUND. Use width(), height(), or size(); clear(color); "
            "color(color); pixel, line, rect, fill_rect, circle, fill_circle, "
            "text; refresh() or present(); then end(). Standard min() and max() "
            "are available. Colors are gfx.WHITE, gfx.LIGHT, gfx.DARK, "
            "gfx.BLACK, and gfx.gray(level); pass these constants to clear() "
            "and color(), never color-name strings or guessed integers. "
            "Required attached-display pattern (replace the quoted target with "
            "a ready display_list name):\n"
            "import solaros\nfrom solaros import gfx\n"
            "gfx.begin(\"verified-ready-target\")\ntry:\n"
            "    gfx.clear(gfx.WHITE)\n    gfx.color(gfx.BLACK)\n"
            "    # draw here\n    gfx.present()\nfinally:\n"
            "    gfx.end()",
    },
    {
        .topic = "lua.gfx",
        .keywords =
            "lua gfx graphics display screen draw pixel line rectangle circle "
            "font oled lcd framebuffer present",
        .reference =
            "Lua: use the preloaded solaros table or local solaros = "
            "require(\"solaros\"), then assign local gfx = solaros.gfx. "
            "gfx.begin() uses the current foreground display and errors from a "
            "port/headless shell where there is none. For an attached display, "
            "the agent must call display_list and pass a returned ready name; "
            "absent names raise ESP_ERR_NOT_FOUND. Use width, height or size; "
            "clear; color; pixel, line, rect, fill_rect, circle, fill_circle, "
            "text; refresh or present. Colors are gfx.WHITE, gfx.LIGHT, "
            "gfx.DARK, gfx.BLACK, and gfx.gray(level); pass these constants to "
            "clear and color, never color-name strings or guessed integers. "
            "Call gfx[\"end\"]() because end is a Lua keyword. Required "
            "attached-display pattern (replace the quoted target with a ready "
            "display_list name):\nlocal solaros = require(\"solaros\")\n"
            "local gfx = solaros.gfx\n"
            "gfx.begin(\"verified-ready-target\")\n"
            "local ok, err = pcall(function()\n"
            "    gfx.clear(gfx.WHITE)\n    gfx.color(gfx.BLACK)\n"
            "    -- draw here\n    gfx.present()\nend)\n"
            "gfx[\"end\"]()\nif not ok then error(err) end",
    },
    {
        .topic = "python.tui",
        .keywords =
            "python py tui terminal text interface curses keyboard keys input "
            "box bold inverse",
        .reference =
            "Python: from solaros import tui. Functions are rows, cols, size, "
            "clear, refresh, move, write, addstr, putch, hline, vline, vrule, "
            "box, fill, and getch. Attributes are NORMAL, BOLD, INVERSE; key "
            "constants include KEY_ESCAPE and navigation keys. Loop while not "
            "solaros.should_exit() for an interactive foreground app.",
    },
    {
        .topic = "lua.tui",
        .keywords =
            "lua tui terminal text interface curses keyboard keys input box "
            "bold inverse",
        .reference =
            "Lua: local tui = solaros.tui. Functions are rows, cols, size, "
            "clear, refresh, move, write, addstr, putch, hline, vline, vrule, "
            "box, fill, and getch. Attributes are NORMAL, BOLD, INVERSE; key "
            "constants include KEY_ESCAPE and navigation keys. Loop while not "
            "solaros.should_exit() for an interactive foreground app.",
    },
    {
        .topic = "storage",
        .keywords =
            "python lua storage filesystem files directories mount sd flash "
            "copy rename remove mkdir path disk volume",
        .reference =
            "Use solaros.storage, not host os or io APIs. Functions include "
            "status, is_mounted, mount, unmount, mount_point, usage, resolve, "
            "rescan, blocks, block_count, block, usage_for_block, mkdir, rmdir, "
            "remove, rename, copy, mount_volume, and unmount_volume. SolarOS "
            "shell paths use slash for the active default storage volume.",
    },
    {
        .topic = "identity",
        .keywords =
            "python lua identity hostname user username nvs wifi advertised "
            "device name",
        .reference =
            "solaros.identity provides user(), hostname(), set_user(name), "
            "set_hostname(name), and format(). Values are NVS-backed. Reboot "
            "before expecting an already initialized Wi-Fi interface to "
            "advertise a changed hostname.",
    },
    {
        .topic = "jobs",
        .keywords =
            "python lua jobs background task process start stop state waiting "
            "failed memory stack",
        .reference =
            "solaros.jobs provides list(), count(), status(name), start(name, "
            "optional_args), and stop(name). Status includes state, last_error, "
            "worker_stack_bytes, worker_stack_external, tick timing, and "
            "deadline telemetry. Waiting means launch admission has not yet "
            "succeeded; failed records a terminal launch or runtime error.",
    },
    {
        .topic = "sessions.apps",
        .keywords =
            "python lua sessions apps shell port terminal create close registry "
            "foreground",
        .reference =
            "solaros.sessions.create_shell(port, optional term, cols, rows) "
            "returns a session id; close(id) closes it. Script-created port "
            "shells do not run /.shell/startup. solaros.apps.list() "
            "and find(name) inspect registered foreground apps.",
    },
    {
        .topic = "network",
        .keywords =
            "python lua wifi wireless station access point ap nat scan connect "
            "mqtt network ping",
        .reference =
            "solaros.wifi provides status, status_text, start, stop, connect, "
            "connect_saved, disconnect, forget, forget_ssid, forget_all, known, "
            "scan, ap_start, ap_stop, and nat. solaros.mqtt provides status, "
            "connect, disconnect, publish, subscribe, and read. solaros.net."
            "ping(host, optional count, timeout_ms, interval_ms, data_size) "
            "returns statistics. These modules are package-gated.",
    },
    {
        .topic = "gpio.analog",
        .keywords =
            "python lua gpio pin digital led adc analog pwm input output pull "
            "read write voltage duty",
        .reference =
            "Inspect solaros.gpio.pins() and allowed(pin) before use; never "
            "invent safe GPIO numbers. GPIO offers mode or configure, read, "
            "write, and release plus INPUT, OUTPUT and pull constants. "
            "solaros.led offers status, set, on, off, toggle. solaros.adc offers "
            "pins and read. solaros.pwm offers status, set(pin, frequency, "
            "duty_percent), and off. APIs are package-gated.",
    },
    {
        .topic = "buses",
        .keywords =
            "python lua buses resource named bus create attach detach i2c spi "
            "uart onewire lease",
        .reference =
            "Prefer solaros.buses for named hardware. list and get inspect "
            "registered buses; create_i2c, create_onewire, create_spi, and "
            "create_uart create runtime buses; attach, detach, remove manage "
            "lifecycle. Transfer families are i2c_probe or scan or read_reg or "
            "write_reg, onewire_reset or scan or xfer, spi_xfer or read or "
            "write, and uart_read or write. Inspect descriptors before choosing "
            "names, pins, chip selects, or hosts.",
    },
    {
        .topic = "compatibility.io",
        .keywords =
            "python lua i2c spi uart onewire serial transfer register probe scan "
            "compatibility",
        .reference =
            "Compatibility modules are solaros.i2c info, probe, scan, read_reg, "
            "write_reg; solaros.spi status, xfer, read, write; solaros.uart "
            "status, baud, is_valid_baud, mode, write, read; solaros.onewire "
            "allowed, reset, scan, xfer. Inspect status or allowed first. New "
            "multi-bus code should prefer solaros.buses. Modules are "
            "package-gated.",
    },
    {
        .topic = "expansion",
        .keywords =
            "python lua expansion device driver attach detach bindings display "
            "oled lcd sensor peripheral",
        .reference =
            "solaros.expansion.drivers() lists compiled drivers and devices() "
            "lists currently attached devices with normalized bindings. "
            "attach(driver, name, bindings) and detach(name) manage them. Never "
            "assume an example name such as lcd0 or oled0 exists; inspect "
            "devices() or use a name explicitly supplied by the user.",
    },
    {
        .topic = "media.input",
        .keywords =
            "python lua audio speaker microphone wav tone ble bluetooth keyboard "
            "clipboard",
        .reference =
            "solaros.audio provides status, deinit or off, set_volume, "
            "set_mic_gain, tone, level, loopback, wav_info, record_wav, and "
            "play_wav. solaros.ble provides status, connected, pair, forget, "
            "layout, read. solaros.clipboard provides set, get, size, clear. "
            "Audio and BLE are package-gated.",
    },
    {
        .topic = "time.sensors",
        .keywords =
            "python lua time clock date timezone ntp uptime battery sensor "
            "temperature humidity environment",
        .reference =
            "solaros.time provides uptime_ms, uptime, datetime, utc_datetime, "
            "set_datetime, set_utc_datetime, utc_to_local, local_to_utc, "
            "is_valid, timezone, set_timezone, and ntp_sync. "
            "solaros.battery.status and solaros.sensors.environment are "
            "package-gated.",
    },
    {
        .topic = "ssh_keys",
        .keywords =
            "python lua ssh scp keys identity private public generate remove "
            "credentials",
        .reference =
            "solaros.ssh_keys provides default_paths, default_exists, status, "
            "generate(optional bits and overwrite), and remove when SSH is "
            "installed. Do not inspect, expose, replace, or delete private key "
            "material unless the user explicitly requests that operation.",
    },
    {
        .topic = "script.conventions",
        .keywords =
            "python lua script conventions import require errors exit arguments "
            "runtime package",
        .reference =
            "Python imports the native solaros module; arguments are in "
            "sys.argv. Lua uses the preloaded global solaros or require with "
            "the module name. Mutating service failures surface as SolarOS "
            "errors. Optional modules are package-gated. Interactive code "
            "should check solaros.should_exit(). Use SolarOS service APIs "
            "instead of assuming Unix process, filesystem, or device APIs.",
    },
};

static const size_t AGENT_REFERENCE_ENTRY_COUNT =
    sizeof(AGENT_REFERENCE_ENTRIES) / sizeof(AGENT_REFERENCE_ENTRIES[0]);

static bool agent_reference_contains_ci(const char *text, const char *needle)
{
    if (text == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }
    const size_t needle_len = strlen(needle);
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        if (strncasecmp(cursor, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool agent_reference_stop_word(const char *token)
{
    static const char *const stop_words[] = {
        "a", "an", "and", "api", "for", "how", "in", "of", "on", "the",
        "to", "use", "with",
    };
    for (size_t i = 0; i < sizeof(stop_words) / sizeof(stop_words[0]); i++) {
        if (strcmp(token, stop_words[i]) == 0) {
            return true;
        }
    }
    return false;
}

static unsigned agent_reference_score(const agent_reference_entry_t *entry,
                                      const char *query)
{
    if (entry == NULL || query == NULL || query[0] == '\0') {
        return 0U;
    }
    if (strcasecmp(entry->topic, query) == 0) {
        return 100000U;
    }

    unsigned score = 0U;
    char token[AGENT_REFERENCE_TOKEN_MAX + 1U];
    size_t token_len = 0U;
    for (const unsigned char *cursor = (const unsigned char *)query;; cursor++) {
        const bool separator = *cursor == '\0' || !isalnum(*cursor);
        if (!separator && token_len < AGENT_REFERENCE_TOKEN_MAX) {
            token[token_len++] = (char)tolower(*cursor);
        }
        if (separator && token_len > 0U) {
            token[token_len] = '\0';
            if (!agent_reference_stop_word(token)) {
                if (agent_reference_contains_ci(entry->topic, token)) {
                    score += 500U;
                }
                if (agent_reference_contains_ci(entry->keywords, token)) {
                    score += 300U;
                }
                if (agent_reference_contains_ci(entry->reference, token)) {
                    score += 20U;
                }
            }
            token_len = 0U;
        }
        if (*cursor == '\0') {
            break;
        }
    }
    return score;
}

static esp_err_t agent_reference_append(agent_reference_output_t *output,
                                        const char *format,
                                        ...)
{
    if (output == NULL || output->buffer == NULL || format == NULL ||
        output->length >= output->capacity) {
        return ESP_ERR_INVALID_ARG;
    }
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(output->buffer + output->length,
                                  output->capacity - output->length,
                                  format,
                                  args);
    va_end(args);
    if (written < 0 ||
        (size_t)written >= output->capacity - output->length) {
        return ESP_ERR_INVALID_SIZE;
    }
    output->length += (size_t)written;
    return ESP_OK;
}

static esp_err_t agent_reference_append_json_string(
    agent_reference_output_t *output,
    const char *text)
{
    esp_err_t err = agent_reference_append(output, "\"");
    if (err != ESP_OK || text == NULL) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_ARG;
    }
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor != '\0';
         cursor++) {
        char escaped[7];
        size_t escaped_len = 1U;
        escaped[0] = (char)*cursor;
        switch (*cursor) {
        case '"':
            memcpy(escaped, "\\\"", 2U);
            escaped_len = 2U;
            break;
        case '\\':
            memcpy(escaped, "\\\\", 2U);
            escaped_len = 2U;
            break;
        case '\b':
            memcpy(escaped, "\\b", 2U);
            escaped_len = 2U;
            break;
        case '\f':
            memcpy(escaped, "\\f", 2U);
            escaped_len = 2U;
            break;
        case '\n':
            memcpy(escaped, "\\n", 2U);
            escaped_len = 2U;
            break;
        case '\r':
            memcpy(escaped, "\\r", 2U);
            escaped_len = 2U;
            break;
        case '\t':
            memcpy(escaped, "\\t", 2U);
            escaped_len = 2U;
            break;
        default:
            if (*cursor < 0x20U) {
                snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
                escaped_len = 6U;
            }
            break;
        }
        if (output->length + escaped_len >= output->capacity) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(output->buffer + output->length, escaped, escaped_len);
        output->length += escaped_len;
        output->buffer[output->length] = '\0';
    }
    return agent_reference_append(output, "\"");
}

esp_err_t solar_os_agent_reference_search(const char *query,
                                          char *result,
                                          size_t result_len)
{
    if (query == NULL || query[0] == '\0' || result == NULL ||
        result_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t indices[AGENT_REFERENCE_MATCH_MAX] = {0};
    unsigned scores[AGENT_REFERENCE_MATCH_MAX] = {0};
    size_t count = 0U;
    for (size_t candidate = 0U;
         candidate < AGENT_REFERENCE_ENTRY_COUNT;
         candidate++) {
        const unsigned score =
            agent_reference_score(&AGENT_REFERENCE_ENTRIES[candidate], query);
        if (score == 0U) {
            continue;
        }
        size_t insert = 0U;
        while (insert < count &&
               (scores[insert] > score ||
                (scores[insert] == score && indices[insert] < candidate))) {
            insert++;
        }
        if (insert >= AGENT_REFERENCE_MATCH_MAX) {
            continue;
        }
        if (count < AGENT_REFERENCE_MATCH_MAX) {
            count++;
        }
        for (size_t move = count - 1U; move > insert; move--) {
            indices[move] = indices[move - 1U];
            scores[move] = scores[move - 1U];
        }
        indices[insert] = candidate;
        scores[insert] = score;
    }
    if (count == 0U) {
        indices[0] = 0U;
        count = 1U;
    }

    agent_reference_output_t output = {
        .buffer = result,
        .capacity = result_len,
    };
    esp_err_t err = agent_reference_append(&output, "{\"guidance\":");
    if (err == ESP_OK) {
        err = agent_reference_append_json_string(&output,
                                                 AGENT_REFERENCE_GUIDANCE);
    }
    if (err == ESP_OK) {
        err = agent_reference_append(&output,
                                     ",\"count\":%u,\"matches\":[",
                                     (unsigned)count);
    }
    for (size_t i = 0U; err == ESP_OK && i < count; i++) {
        const agent_reference_entry_t *entry =
            &AGENT_REFERENCE_ENTRIES[indices[i]];
        err = agent_reference_append(&output,
                                     "%s{\"topic\":",
                                     i == 0U ? "" : ",");
        if (err == ESP_OK) {
            err = agent_reference_append_json_string(&output, entry->topic);
        }
        if (err == ESP_OK) {
            err = agent_reference_append(&output, ",\"reference\":");
        }
        if (err == ESP_OK) {
            err = agent_reference_append_json_string(&output,
                                                     entry->reference);
        }
        if (err == ESP_OK) {
            err = agent_reference_append(&output, "}");
        }
    }
    if (err == ESP_OK) {
        err = agent_reference_append(&output, "]}");
    }
    return err;
}
