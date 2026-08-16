#include "solar_os_ag_tarayici.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_net.h"
#include "solar_os_task.h"
#include "solar_os_wifi.h"

#define TAG "ag_tarayici"

#define AGTARA_MAX_HOSTS 64U
#define AGTARA_MAX_OPEN_PORTS 8U
#define AGTARA_PING_TIMEOUT_MS 120U
#define AGTARA_PORT_TIMEOUT_MS 150U
#define AGTARA_TASK_STACK 6144U
#define AGTARA_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define AGTARA_TICK_MS 100U

#define AGTARA_HEADER_H 24
#define AGTARA_FOOTER_H 22

typedef enum {
    AGTARA_VIEW_LIST = 0,
    AGTARA_VIEW_DETAIL = 1,
} agtara_view_t;

typedef struct {
    char ip[16];
    uint16_t open_ports[AGTARA_MAX_OPEN_PORTS];
    size_t open_port_count;
} agtara_host_t;

typedef struct {
    /* --- Scan worker (guarded by agtara_lock) --- */
    TaskHandle_t task;
    volatile bool task_done;
    volatile bool stop_requested;
    agtara_host_t staging_host;
    bool staging_ready;
    uint32_t staging_progress;

    /* --- Main thread state --- */
    bool scanning;
    uint32_t scan_progress;
    char base_prefix[16];
    uint8_t own_last_octet;
    char local_ip[16];

    agtara_host_t hosts[AGTARA_MAX_HOSTS];
    size_t host_count;
    size_t selected;

    agtara_view_t view;
    uint32_t elapsed_ms;
    bool render_pending;
    char status_message[64];
    uint32_t status_until_ms;
} agtara_state_t;

static void *agtara_state_ptr;
#define agtara (*(agtara_state_t *)agtara_state_ptr)

SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("ag_tarayici scan worker spinlock, no app data")
static portMUX_TYPE agtara_lock = portMUX_INITIALIZER_UNLOCKED;

static void agtara_render(solar_os_context_t *ctx);
static void agtara_set_status(const char *message);

static const uint16_t AGTARA_PORTS[] = {
    80, 443, 22, 23, 21, 53, 8080, 8000, 8443, 1883, 3000, 5000, 8888, 9000,
};
#define AGTARA_PORT_COUNT (sizeof(AGTARA_PORTS) / sizeof(AGTARA_PORTS[0]))

static const char *agtara_port_name(uint16_t port)
{
    switch (port) {
    case 80: return "HTTP";
    case 443: return "HTTPS";
    case 22: return "SSH";
    case 23: return "Telnet";
    case 21: return "FTP";
    case 53: return "DNS";
    case 8080: return "HTTP-Alt";
    case 8000: return "HTTP-Dev";
    case 8443: return "HTTPS-Alt";
    case 1883: return "MQTT";
    case 3000: return "Node/Dev";
    case 5000: return "Flask/UPnP";
    case 8888: return "HTTP-Proxy";
    case 9000: return "Portainer";
    default: return "";
    }
}

static bool agtara_should_stop(void *user)
{
    (void)user;
    return agtara.stop_requested;
}

static bool agtara_probe_tcp(const char *ip, uint16_t port, uint32_t timeout_ms)
{
    const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) return false;

    struct timeval tv = {
        .tv_sec = (time_t)(timeout_ms / 1000U),
        .tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U),
    };
    (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    (void)inet_pton(AF_INET, ip, &addr.sin_addr);

    const int res = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    close(s);
    return res == 0;
}

static void agtara_worker(void *arg)
{
    (void)arg;
    char base[16];
    strncpy(base, agtara.base_prefix, sizeof(base) - 1U);
    base[sizeof(base) - 1U] = '\0';
    const uint8_t own_last = agtara.own_last_octet;

    for (int d = 1; d <= 254; d++) {
        if (agtara.stop_requested) break;

        portENTER_CRITICAL(&agtara_lock);
        agtara.staging_progress = (uint32_t)d;
        portEXIT_CRITICAL(&agtara_lock);

        if ((uint8_t)d == own_last) continue;

        char ip[16];
        snprintf(ip, sizeof(ip), "%s.%d", base, d);

        const solar_os_net_ping_options_t popt = {
            .count = 1U,
            .timeout_ms = AGTARA_PING_TIMEOUT_MS,
            .interval_ms = 0U,
            .data_size = 32U,
        };
        solar_os_net_ping_result_t pres = {0};
        const esp_err_t perr =
            solar_os_net_ping(ip, &popt, NULL, NULL, agtara_should_stop, NULL, &pres);

        if (agtara.stop_requested) break;
        if (perr != ESP_OK || pres.received == 0U) continue;

        agtara_host_t host = {0};
        strncpy(host.ip, ip, sizeof(host.ip) - 1U);
        for (size_t pi = 0U; pi < AGTARA_PORT_COUNT && !agtara.stop_requested; pi++) {
            if (host.open_port_count >= AGTARA_MAX_OPEN_PORTS) break;
            if (agtara_probe_tcp(ip, AGTARA_PORTS[pi], AGTARA_PORT_TIMEOUT_MS)) {
                host.open_ports[host.open_port_count++] = AGTARA_PORTS[pi];
            }
        }

        portENTER_CRITICAL(&agtara_lock);
        agtara.staging_host = host;
        agtara.staging_ready = true;
        portEXIT_CRITICAL(&agtara_lock);

        while (agtara.staging_ready && !agtara.stop_requested) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    portENTER_CRITICAL(&agtara_lock);
    agtara.task_done = true;
    portEXIT_CRITICAL(&agtara_lock);

    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void agtara_start_scan(void)
{
    if (agtara.scanning) return;

    solar_os_wifi_status_t wifi_status;
    solar_os_wifi_get_status(&wifi_status);
    if (!wifi_status.connected || !wifi_status.has_ip) {
        agtara_set_status("Connect to Wi-Fi first");
        return;
    }
    int a = 0, b = 0, c = 0, d = 0;
    if (sscanf(wifi_status.ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
        agtara_set_status("Could not parse local IP");
        return;
    }
    snprintf(agtara.base_prefix, sizeof(agtara.base_prefix), "%d.%d.%d", a, b, c);
    agtara.own_last_octet = (uint8_t)d;
    strncpy(agtara.local_ip, wifi_status.ip, sizeof(agtara.local_ip) - 1U);

    agtara.host_count = 0U;
    agtara.selected = 0U;
    agtara.scan_progress = 0U;
    agtara.staging_progress = 0U;
    agtara.staging_ready = false;
    agtara.stop_requested = false;
    agtara.task_done = false;

    const BaseType_t created = solar_os_task_create_pinned_internal(
        agtara_worker, "agtara_scan", AGTARA_TASK_STACK, NULL, AGTARA_TASK_PRIORITY,
        &agtara.task, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created == pdPASS) {
        agtara.scanning = true;
        agtara_set_status("Scanning LAN subnet...");
    } else {
        agtara.task = NULL;
        agtara_set_status("Scan task creation failed");
    }
}

static void agtara_drain(void)
{
    bool ready = false;
    agtara_host_t host = {0};
    uint32_t progress;

    portENTER_CRITICAL(&agtara_lock);
    ready = agtara.staging_ready;
    if (ready) {
        host = agtara.staging_host;
        agtara.staging_ready = false;
    }
    progress = agtara.staging_progress;
    portEXIT_CRITICAL(&agtara_lock);

    agtara.scan_progress = progress;
    if (ready && agtara.host_count < AGTARA_MAX_HOSTS) {
        agtara.hosts[agtara.host_count++] = host;
        agtara.render_pending = true;
    }
}

static void agtara_reap(void)
{
    if (agtara.task == NULL || !agtara.task_done) return;
    agtara_drain();
    solar_os_task_delete(agtara.task);
    agtara.task = NULL;
    agtara.scanning = false;
    agtara_set_status("LAN Scan completed");
}

static void agtara_set_status(const char *message)
{
    strncpy(agtara.status_message, message, sizeof(agtara.status_message) - 1U);
    agtara.status_message[sizeof(agtara.status_message) - 1U] = '\0';
    agtara.status_until_ms = agtara.elapsed_ms + 2500U;
    agtara.render_pending = true;
}

/* ---------------------------------------------------------------------
 * UI Rendering
 * ------------------------------------------------------------------- */

static void agtara_draw_header(solar_os_gfx_t *gfx, int width)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, width, AGTARA_HEADER_H);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);

    char header[80];
    snprintf(header, sizeof(header), "NETWORK SCANNER - %s",
             agtara.view == AGTARA_VIEW_LIST ? "ACTIVE HOSTS" : "PORT DETAILS");
    solar_os_gfx_text(gfx, 8, 16, header);
}

static void agtara_draw_footer(solar_os_gfx_t *gfx, int width, int height)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, height - AGTARA_FOOTER_H, width, AGTARA_FOOTER_H);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);

    char footer[120];
    if (agtara.status_until_ms > agtara.elapsed_ms && agtara.status_message[0] != '\0') {
        snprintf(footer, sizeof(footer), "%s", agtara.status_message);
    } else if (agtara.view == AGTARA_VIEW_LIST) {
        snprintf(footer, sizeof(footer), "[S] Start Scan | [Space] Stop | [Up/Down] Select | [Enter] Ports | [ESC] Exit");
    } else {
        snprintf(footer, sizeof(footer), "[Backspace/Left] Back to Host List | [ESC] Exit");
    }
    solar_os_gfx_text(gfx, 8, height - 6, footer);
}

static void agtara_draw_list(solar_os_gfx_t *gfx, int width, int height)
{
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    char info[96];
    if (agtara.local_ip[0] != '\0') {
        snprintf(info, sizeof(info), "Local IP: %s   Subnet: %s.0/24", agtara.local_ip, agtara.base_prefix);
    } else {
        snprintf(info, sizeof(info), "Press [S] to start scanning LAN hosts");
    }
    solar_os_gfx_text(gfx, 8, AGTARA_HEADER_H + 16, info);

    if (agtara.scanning) {
        char progress[64];
        snprintf(progress, sizeof(progress), "Scanning: %u/254 hosts checked (%u alive found)",
                 (unsigned)agtara.scan_progress, (unsigned)agtara.host_count);
        solar_os_gfx_text(gfx, 8, AGTARA_HEADER_H + 32, progress);

        const int bar_x = 8;
        const int bar_y = AGTARA_HEADER_H + 38;
        const int bar_w = width - 16;
        solar_os_gfx_rect(gfx, bar_x, bar_y, bar_w, 8);
        const int fill_w = (int)((float)(bar_w - 2) * (float)agtara.scan_progress / 254.0f);
        if (fill_w > 0) {
            solar_os_gfx_fill_rect(gfx, bar_x + 1, bar_y + 1, fill_w, 6);
        }
    }

    const int row_h = 22;
    const int top = AGTARA_HEADER_H + (agtara.scanning ? 54 : 30);
    const int max_rows = (height - AGTARA_FOOTER_H - top) / row_h;
    if (max_rows <= 0) return;

    size_t start = 0U;
    if (agtara.host_count > (size_t)max_rows && agtara.selected >= (size_t)max_rows) {
        start = agtara.selected - (size_t)max_rows + 1U;
    }

    for (int row = 0; row < max_rows; row++) {
        const size_t idx = start + (size_t)row;
        if (idx >= agtara.host_count) break;

        const int y = top + row * row_h;
        const bool is_sel = (idx == agtara.selected);
        const agtara_host_t *host = &agtara.hosts[idx];

        if (is_sel) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, 4, y, width - 8, row_h - 1);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        char line[64];
        if (host->open_port_count > 0U) {
            snprintf(line, sizeof(line), "● %s  (%u open ports)", host->ip, (unsigned)host->open_port_count);
        } else {
            snprintf(line, sizeof(line), "● %s  (Ping OK, no common ports)", host->ip);
        }
        solar_os_gfx_text(gfx, 10, y + row_h - 6, line);
    }
}

static void agtara_draw_detail(solar_os_gfx_t *gfx, int width, int height)
{
    if (agtara.selected >= agtara.host_count) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 14, AGTARA_HEADER_H + 24, "No host selected");
        return;
    }
    const agtara_host_t *host = &agtara.hosts[agtara.selected];

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
    solar_os_gfx_text(gfx, 14, AGTARA_HEADER_H + 30, host->ip);
    solar_os_gfx_line(gfx, 14, AGTARA_HEADER_H + 36, width - 14, AGTARA_HEADER_H + 36);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_16);
    if (host->open_port_count == 0U) {
        solar_os_gfx_text(gfx, 16, AGTARA_HEADER_H + 64, "No common open TCP ports detected");
        return;
    }

    int y = AGTARA_HEADER_H + 64;
    for (size_t i = 0U; i < host->open_port_count; i++) {
        char line[48];
        const char *name = agtara_port_name(host->open_ports[i]);
        if (name[0] != '\0') {
            snprintf(line, sizeof(line), "Port %-5u [%s]  OPEN", (unsigned)host->open_ports[i], name);
        } else {
            snprintf(line, sizeof(line), "Port %-5u OPEN", (unsigned)host->open_ports[i]);
        }
        solar_os_gfx_text(gfx, 16, y, line);
        y += 24;
        if (y > height - AGTARA_FOOTER_H - 12) break;
    }
}

static void agtara_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    agtara_draw_header(gfx, width);

    if (agtara.view == AGTARA_VIEW_DETAIL) {
        agtara_draw_detail(gfx, width, height);
    } else {
        agtara_draw_list(gfx, width, height);
    }

    agtara_draw_footer(gfx, width, height);
    solar_os_gfx_present(gfx);
    agtara.render_pending = false;
}

static void agtara_handle_char(solar_os_context_t *ctx, char ch)
{
    const unsigned char uch = (unsigned char)ch;

    if (uch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
        solar_os_context_request_exit(ctx);
        return;
    }
    if (uch == 0x7fU || uch == 0x08U || uch == SOLAR_OS_KEY_LEFT) {
        if (agtara.view == AGTARA_VIEW_DETAIL) {
            agtara.view = AGTARA_VIEW_LIST;
            agtara.render_pending = true;
        }
        return;
    }
    if (ch == 's' || ch == 'S') {
        agtara_start_scan();
        return;
    }
    if (ch == ' ') {
        if (agtara.scanning) {
            agtara.stop_requested = true;
            agtara_set_status("Canceling scan...");
        }
        return;
    }

    if (agtara.view == AGTARA_VIEW_LIST) {
        if (uch == SOLAR_OS_KEY_UP || ch == 'w' || ch == 'W') {
            if (agtara.selected > 0U) {
                agtara.selected--;
                agtara.render_pending = true;
            }
        } else if (uch == SOLAR_OS_KEY_DOWN || ch == 's' || ch == 'S') {
            if (agtara.selected + 1U < agtara.host_count) {
                agtara.selected++;
                agtara.render_pending = true;
            }
        } else if (ch == '\n' || ch == '\r') {
            if (agtara.selected < agtara.host_count) {
                agtara.view = AGTARA_VIEW_DETAIL;
                agtara.render_pending = true;
            }
        }
    }
}

static bool agtara_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    switch (event->type) {
    case SOLAR_OS_EVENT_CHAR:
        agtara_handle_char(ctx, event->data.ch);
        break;
    case SOLAR_OS_EVENT_TICK:
        agtara.elapsed_ms += AGTARA_TICK_MS;
        if (agtara.scanning) {
            agtara_drain();
        }
        agtara_reap();
        if (agtara.status_until_ms != 0U && agtara.status_until_ms <= agtara.elapsed_ms &&
            agtara.status_until_ms + AGTARA_TICK_MS > agtara.elapsed_ms) {
            agtara.render_pending = true;
        }
        if (agtara.render_pending) {
            agtara_render(ctx);
        }
        break;
    case SOLAR_OS_EVENT_RESUME:
        agtara.render_pending = true;
        agtara_render(ctx);
        break;
    default:
        break;
    }
    return true;
}

static esp_err_t agtara_start(solar_os_context_t *ctx)
{
    memset(&agtara, 0, sizeof(agtara));
    agtara.view = AGTARA_VIEW_LIST;
    agtara.render_pending = true;

    solar_os_context_set_graphics_active(ctx, true);
    agtara_render(ctx);
    return ESP_OK;
}

static void agtara_suspend(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static void agtara_resume(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, true);
    agtara.render_pending = true;
    agtara_render(ctx);
}

static void agtara_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (agtara.task != NULL) {
        agtara.stop_requested = true;
        vTaskDelay(pdMS_TO_TICKS(50));
        solar_os_task_delete(agtara.task);
        agtara.task = NULL;
    }
}

static void agtara_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    snprintf(buffer, buffer_len, "Scanner: %u hosts", (unsigned)agtara.host_count);
}

const solar_os_app_t solar_os_ag_tarayici_app = {
    .name = "ag_tarayici",
    .summary = "LAN host and open port scanner",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = agtara_start,
    .suspend = agtara_suspend,
    .resume = agtara_resume,
    .stop = agtara_stop,
    .event = agtara_event,
    .title = agtara_title,
    .state_slot = &agtara_state_ptr,
    .state_size = sizeof(agtara_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = AGTARA_TASK_STACK,
    .worker_stack_external = false,
    .tick_interval_ms = AGTARA_TICK_MS,
    .tick_deadline_ms = AGTARA_TICK_MS * 2U,
};
