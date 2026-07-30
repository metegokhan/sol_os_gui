#include "solar_os_shell_commands.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_link.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static void link_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  link status|list");
    solar_os_shell_io_writeln(term, "  link status <link>");
    solar_os_shell_io_writeln(term, "  link send <link> <broadcast|destination-id> <text>");
    solar_os_shell_io_writeln(term,
                              "  link send-binary <link> <broadcast|destination-id> <byte...>");
    solar_os_shell_io_writeln(term, "  link receive <link> [timeout-ms]");
}

static bool parse_u32(const char *text, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool parse_destination(const char *text, uint32_t *destination)
{
    if (strcmp(text, "broadcast") == 0) {
        *destination = SOLAR_OS_LINK_BROADCAST;
        return true;
    }
    return parse_u32(text, destination) && *destination != 0 &&
           *destination != SOLAR_OS_LINK_BROADCAST;
}

static bool parse_byte(const char *text, uint8_t *value)
{
    uint32_t parsed = 0;
    if (!parse_u32(text, &parsed) || parsed > UINT8_MAX) {
        return false;
    }
    *value = (uint8_t)parsed;
    return true;
}

static void link_print_error(solar_os_shell_io_t *term, const char *operation, esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NOT_FOUND:
        solar_os_shell_io_printf(term, "%s: link not found\n", operation);
        break;
    case ESP_ERR_INVALID_SIZE:
        solar_os_shell_io_printf(term, "%s: payload exceeds transport MTU\n", operation);
        break;
    case ESP_ERR_NO_MEM:
        solar_os_shell_io_printf(term, "%s: queue full\n", operation);
        break;
    case ESP_ERR_TIMEOUT:
        solar_os_shell_io_printf(term, "%s: no message\n", operation);
        break;
    default:
        solar_os_shell_io_printf(term, "%s failed: %s\n", operation, esp_err_to_name(err));
        break;
    }
}

static void link_print_status(solar_os_shell_io_t *term, const solar_os_link_status_t *status)
{
    const size_t payload_mtu =
        status->frame_mtu >= SOLAR_OS_LINK_HEADER_SIZE + SOLAR_OS_LINK_CRC_SIZE
            ? status->frame_mtu - SOLAR_OS_LINK_HEADER_SIZE - SOLAR_OS_LINK_CRC_SIZE
            : 0;
    solar_os_shell_io_printf(term,
                             "%s id=0x%08" PRIx32 " mtu=%u payload=%u rx-queued=%u tx-queued=%u"
                             " ack-pending=%u\n",
                             status->name,
                             status->local_id,
                             (unsigned)status->frame_mtu,
                             (unsigned)payload_mtu,
                             (unsigned)status->rx_queued,
                             (unsigned)status->tx_queued,
                             (unsigned)status->acknowledgements_pending);
    solar_os_shell_io_printf(term,
                             "  tx=%" PRIu32 " rx=%" PRIu32 " duplicates=%" PRIu32
                             " crc-errors=%" PRIu32 " ack-sent=%" PRIu32 " ack-received=%" PRIu32
                             " dropped=%" PRIu32 " last=%s\n",
                             status->tx_messages,
                             status->rx_messages,
                             status->duplicates,
                             status->crc_errors,
                             status->acknowledgements_sent,
                             status->acknowledgements_received,
                             status->dropped,
                             esp_err_to_name(status->last_error));
}

static void link_status(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc == 2) {
        const size_t count = solar_os_link_count();
        if (count == 0) {
            solar_os_shell_io_writeln(term, "no active links");
            return;
        }
        for (size_t i = 0; i < count; i++) {
            solar_os_link_status_t status;
            if (solar_os_link_get(i, &status)) {
                link_print_status(term, &status);
            }
        }
        return;
    }
    if (argc != 3) {
        solar_os_shell_io_writeln(term, "usage: link status <link>");
        return;
    }
    solar_os_link_status_t status;
    const esp_err_t ret = solar_os_link_get_status(argv[2], &status);
    if (ret != ESP_OK) {
        link_print_error(term, "link status", ret);
        return;
    }
    link_print_status(term, &status);
}

static void link_send_text(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc < 5) {
        solar_os_shell_io_writeln(term,
                                  "usage: link send <link> <broadcast|destination-id> <text>");
        return;
    }
    uint32_t destination = 0;
    if (!parse_destination(argv[3], &destination)) {
        link_print_error(term, "link send", ESP_ERR_INVALID_ARG);
        return;
    }

    uint8_t payload[SOLAR_OS_LINK_PAYLOAD_MAX];
    size_t used = 0;
    for (int i = 4; i < argc; i++) {
        const size_t len = strlen(argv[i]);
        if (used + len + (i > 4 ? 1U : 0U) > sizeof(payload)) {
            link_print_error(term, "link send", ESP_ERR_INVALID_SIZE);
            return;
        }
        if (i > 4) {
            payload[used++] = ' ';
        }
        memcpy(&payload[used], argv[i], len);
        used += len;
    }

    uint16_t sequence = 0;
    const esp_err_t ret = solar_os_link_send(
        argv[2], SOLAR_OS_LINK_MESSAGE_TEXT, destination, payload, used, &sequence);
    if (ret != ESP_OK) {
        link_print_error(term, "link send", ret);
        return;
    }
    solar_os_shell_io_printf(term, "queued sequence=%u\n", sequence);
}

static void link_send_binary(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc < 5) {
        solar_os_shell_io_writeln(
            term, "usage: link send-binary <link> <broadcast|destination-id> <byte...>");
        return;
    }
    uint32_t destination = 0;
    if (!parse_destination(argv[3], &destination)) {
        link_print_error(term, "link send-binary", ESP_ERR_INVALID_ARG);
        return;
    }

    uint8_t payload[SOLAR_OS_LINK_PAYLOAD_MAX];
    size_t len = 0;
    for (int i = 4; i < argc; i++) {
        if (len >= sizeof(payload) || !parse_byte(argv[i], &payload[len++])) {
            link_print_error(term, "link send-binary", ESP_ERR_INVALID_ARG);
            return;
        }
    }
    uint16_t sequence = 0;
    const esp_err_t ret = solar_os_link_send(
        argv[2], SOLAR_OS_LINK_MESSAGE_BINARY, destination, payload, len, &sequence);
    if (ret != ESP_OK) {
        link_print_error(term, "link send-binary", ret);
        return;
    }
    solar_os_shell_io_printf(term, "queued sequence=%u\n", sequence);
}

static void link_receive(solar_os_shell_io_t *term, int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        solar_os_shell_io_writeln(term, "usage: link receive <link> [timeout-ms]");
        return;
    }
    uint32_t timeout_ms = 0;
    if (argc == 4 && (!parse_u32(argv[3], &timeout_ms) || timeout_ms > 1000U)) {
        link_print_error(term, "link receive", ESP_ERR_INVALID_ARG);
        return;
    }

    solar_os_link_message_t message;
    const esp_err_t ret = solar_os_link_receive(argv[2], &message, timeout_ms);
    if (ret != ESP_OK) {
        link_print_error(term, "link receive", ret);
        return;
    }

    if (message.destination == SOLAR_OS_LINK_BROADCAST) {
        solar_os_shell_io_printf(
            term,
            "type=%s sequence=%u source=0x%08" PRIx32
            " destination=broadcast length=%u\n",
            solar_os_link_message_type_name(message.type),
            message.sequence,
            message.source,
            (unsigned)message.payload_len);
    } else {
        solar_os_shell_io_printf(
            term,
            "type=%s sequence=%u source=0x%08" PRIx32
            " destination=0x%08" PRIx32 " length=%u\n",
            solar_os_link_message_type_name(message.type),
            message.sequence,
            message.source,
            message.destination,
            (unsigned)message.payload_len);
    }
    if (message.type == SOLAR_OS_LINK_MESSAGE_TEXT) {
        solar_os_shell_io_printf(
            term, "%.*s\n", (int)message.payload_len, (const char *)message.payload);
    } else {
        for (size_t i = 0; i < message.payload_len; i++) {
            solar_os_shell_io_printf(term, "%s%02x", i == 0 ? "" : " ", message.payload[i]);
        }
        solar_os_shell_io_put_char(term, '\n');
    }
}

void solar_os_shell_cmd_link(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 1 ||
        (argc == 2 && (strcmp(argv[1], "status") == 0 || strcmp(argv[1], "list") == 0))) {
        link_status(term, 2, argv);
    } else if (strcmp(argv[1], "status") == 0) {
        link_status(term, argc, argv);
    } else if (strcmp(argv[1], "send") == 0) {
        link_send_text(term, argc, argv);
    } else if (strcmp(argv[1], "send-binary") == 0) {
        link_send_binary(term, argc, argv);
    } else if (strcmp(argv[1], "receive") == 0 || strcmp(argv[1], "recv") == 0) {
        link_receive(term, argc, argv);
    } else {
        link_usage(term);
    }
}
