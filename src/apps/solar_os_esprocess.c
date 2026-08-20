#include "solar_os_esprocess.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "solar_os.h"
#include "solar_os_appbar.h"
#include "solar_os_app_registry.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_sessions.h"
#include "solar_os_jobs.h"
#include "solar_os_task.h"
#include "solar_os_time.h"

#define ESPROCESS_TAB_COUNT 4
#define ESPROCESS_MAX_SESSIONS 8
#define ESPROCESS_MAX_TASKS 32
#define ESPROCESS_MAX_JOBS 16
#define ESPROCESS_AUTO_REFRESH_MS 5000U

typedef enum {
    ESPROCESS_TAB_SESSIONS = 0,
    ESPROCESS_TAB_TASKS = 1,
    ESPROCESS_TAB_MEMORY = 2,
    ESPROCESS_TAB_JOBS = 3,
} esprocess_tab_t;

typedef struct {
    char name[16];
    char state_char;
    uint32_t priority;
    uint32_t cpu_tenths;
    uint32_t stack_free_bytes;
} esprocess_task_row_t;

typedef struct {
    solar_os_context_t *ctx;
    esprocess_tab_t tab;
    size_t selected_session;
    size_t selected_task;
    size_t selected_job;
    size_t session_scroll_offset;
    size_t task_scroll_offset;
    size_t job_scroll_offset;

    solar_os_session_info_t sessions[ESPROCESS_MAX_SESSIONS];
    size_t session_count;

    esprocess_task_row_t tasks[ESPROCESS_MAX_TASKS];
    size_t task_count;
    uint32_t total_runtime_sample;

    solar_os_job_status_t jobs[ESPROCESS_MAX_JOBS];
    size_t job_count;

    char notice_msg[64];
    uint32_t notice_until_ms;
    uint32_t last_refresh_ms;
    bool show_help;
} esprocess_state_t;

static void *esprocess_state_ptr;
#define epstate (*(esprocess_state_t *)esprocess_state_ptr)

static const char *const tab_names[ESPROCESS_TAB_COUNT] = {
    "[1] Processes",
    "[2] Tasks",
    "[3] Memory/CPU",
    "[4] Services"
};

static void format_bytes(uint64_t bytes, char *buffer, size_t buffer_len)
{
    static const char *units[] = {"B", "KB", "MB", "GB"};
    size_t unit_index = 0;
    uint64_t scale = 1;

    while (unit_index + 1 < sizeof(units) / sizeof(units[0]) &&
           bytes >= scale * 1024ULL) {
        scale *= 1024ULL;
        unit_index++;
    }

    if (unit_index == 0) {
        snprintf(buffer, buffer_len, "%" PRIu64 " B", bytes);
        return;
    }

    const uint64_t tenths = ((bytes * 10ULL) + (scale / 2ULL)) / scale;
    snprintf(buffer, buffer_len, "%" PRIu64 ".%u %s",
             tenths / 10ULL, (unsigned)(tenths % 10ULL), units[unit_index]);
}

static char task_state_char(eTaskState state)
{
    switch (state) {
    case eRunning:   return 'R';
    case eReady:     return 'r';
    case eBlocked:   return 'B';
    case eSuspended: return 'S';
    case eDeleted:   return 'D';
    default:         return '?';
    }
}

static void esprocess_refresh_data(void)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
    epstate.last_refresh_ms = now;

    /* 1. Refresh Sessions */
    epstate.session_count = solar_os_sessions_get_all_info(epstate.sessions, ESPROCESS_MAX_SESSIONS);
    if (epstate.selected_session >= epstate.session_count && epstate.session_count > 0) {
        epstate.selected_session = epstate.session_count - 1;
    }
    if (epstate.session_scroll_offset > epstate.selected_session) {
        epstate.session_scroll_offset = epstate.selected_session;
    }

    /* 2. Refresh FreeRTOS Tasks */
#if (configUSE_TRACE_FACILITY == 1)
    TaskStatus_t raw_tasks[ESPROCESS_MAX_TASKS];
    const UBaseType_t task_cap = ESPROCESS_MAX_TASKS;
#if (configGENERATE_RUN_TIME_STATS == 1)
    configRUN_TIME_COUNTER_TYPE total_runtime = 0;
    UBaseType_t count = uxTaskGetSystemState(raw_tasks, task_cap, &total_runtime);
#else
    UBaseType_t count = uxTaskGetSystemState(raw_tasks, task_cap, NULL);
    uint32_t total_runtime = 0;
#endif
    if (count > ESPROCESS_MAX_TASKS) count = ESPROCESS_MAX_TASKS;

    /* Sort tasks by CPU / Runtime descending */
    for (UBaseType_t i = 0; i < count; i++) {
        for (UBaseType_t j = i + 1; j < count; j++) {
            if (raw_tasks[j].ulRunTimeCounter > raw_tasks[i].ulRunTimeCounter) {
                TaskStatus_t tmp = raw_tasks[i];
                raw_tasks[i] = raw_tasks[j];
                raw_tasks[j] = tmp;
            }
        }
    }

    epstate.task_count = count;
    epstate.total_runtime_sample = (uint32_t)total_runtime;

    for (size_t i = 0; i < count; i++) {
        strlcpy(epstate.tasks[i].name, raw_tasks[i].pcTaskName ? raw_tasks[i].pcTaskName : "?", sizeof(epstate.tasks[i].name));
        epstate.tasks[i].state_char = task_state_char(raw_tasks[i].eCurrentState);
        epstate.tasks[i].priority = (uint32_t)raw_tasks[i].uxCurrentPriority;
        epstate.tasks[i].stack_free_bytes = (uint32_t)raw_tasks[i].usStackHighWaterMark * sizeof(StackType_t);

#if (configGENERATE_RUN_TIME_STATS == 1)
        epstate.tasks[i].cpu_tenths = total_runtime > 0 ?
            (uint32_t)((((uint64_t)raw_tasks[i].ulRunTimeCounter * 1000ULL) + (total_runtime / 2ULL)) / total_runtime) : 0;
#else
        epstate.tasks[i].cpu_tenths = 0;
#endif
    }
    if (epstate.selected_task >= epstate.task_count && epstate.task_count > 0) {
        epstate.selected_task = epstate.task_count - 1;
    }
    if (epstate.task_scroll_offset > epstate.selected_task) {
        epstate.task_scroll_offset = epstate.selected_task;
    }
#endif

    /* 3. Refresh OS Jobs */
    const size_t total_jobs = solar_os_jobs_count();
    epstate.job_count = total_jobs > ESPROCESS_MAX_JOBS ? ESPROCESS_MAX_JOBS : total_jobs;
    for (size_t i = 0; i < epstate.job_count; i++) {
        solar_os_jobs_get(i, &epstate.jobs[i]);
    }
    if (epstate.selected_job >= epstate.job_count && epstate.job_count > 0) {
        epstate.selected_job = epstate.job_count - 1;
    }
    if (epstate.job_scroll_offset > epstate.selected_job) {
        epstate.job_scroll_offset = epstate.selected_job;
    }
}

static void draw_progress_bar(solar_os_gfx_t *gfx, int x, int y, int w, int h, int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, w, h);

    const int fill_w = (w - 4) * percent / 100;
    if (fill_w > 0) {
        solar_os_gfx_fill_rect(gfx, x + 2, y + 2, fill_w, h - 4);
    }
}

static void draw_scrollbar(solar_os_gfx_t *gfx, int x, int y, int h, size_t total_count, size_t visible_count, size_t offset)
{
    if (total_count <= visible_count || visible_count == 0 || h <= 10) {
        return;
    }

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, x, y, x, y + h);

    int thumb_h = (int)((visible_count * (size_t)h) / total_count);
    if (thumb_h < 8) thumb_h = 8;
    if (thumb_h > h) thumb_h = h;

    const size_t max_offset = total_count - visible_count;
    int thumb_y = y;
    if (max_offset > 0) {
        thumb_y = y + (int)((offset * (size_t)(h - thumb_h)) / max_offset);
    }

    solar_os_gfx_fill_rect(gfx, x - 2, thumb_y, 4, thumb_h);
}

static void adjust_scroll(size_t *selected, size_t *offset, size_t total_count, size_t visible_rows, int delta)
{
    if (total_count == 0) {
        *selected = 0;
        *offset = 0;
        return;
    }

    if (delta > 0) {
        /* Moving Down */
        const size_t step = (size_t)delta;
        if (*selected + step < total_count) {
            *selected += step;
        } else {
            *selected = total_count - 1;
        }
    } else if (delta < 0) {
        /* Moving Up */
        const size_t step = (size_t)(-delta);
        if (*selected >= step) {
            *selected -= step;
        } else {
            *selected = 0;
        }
    }

    /* Keep selected within visible window */
    if (*selected < *offset) {
        *offset = *selected;
    } else if (*selected >= *offset + visible_rows) {
        *offset = *selected - visible_rows + 1;
    }
}

static void esprocess_draw_tab_sessions(solar_os_gfx_t *gfx, int top_y, int bottom_y)
{
    const int table_y = top_y + 4;
    const int row_h = 24;
    const size_t visible_rows = (size_t)((bottom_y - table_y - 20) / row_h);

    /* Header */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 6, table_y, 388, 18);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 10, table_y + 13, "ID");
    solar_os_gfx_text(gfx, 36, table_y + 13, "APPLICATION");
    solar_os_gfx_text(gfx, 140, table_y + 13, "TITLE");
    solar_os_gfx_text(gfx, 260, table_y + 13, "STATUS");
    solar_os_gfx_text(gfx, 340, table_y + 13, "INTERVAL");

    int cur_y = table_y + 20;

    if (epstate.session_count == 0) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 110, cur_y + 30, "No active processes found");
        return;
    }

    const size_t start_idx = epstate.session_scroll_offset;
    const size_t end_idx = (start_idx + visible_rows < epstate.session_count) ?
                           (start_idx + visible_rows) : epstate.session_count;

    for (size_t i = start_idx; i < end_idx; i++) {
        const solar_os_session_info_t *s = &epstate.sessions[i];
        const bool is_sel = (i == epstate.selected_session);

        if (is_sel) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, 6, cur_y, 382, row_h - 2);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_line(gfx, 6, cur_y + row_h - 2, 388, cur_y + row_h - 2);
        }

        char id_buf[8];
        snprintf(id_buf, sizeof(id_buf), "#%u", (unsigned)s->id);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 10, cur_y + 16, id_buf);

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 36, cur_y + 16, s->app_name);

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        char short_title[22];
        strlcpy(short_title, s->title[0] ? s->title : "-", sizeof(short_title));
        solar_os_gfx_text(gfx, 140, cur_y + 16, short_title);

        const char *st_str = s->is_foreground ? "FOREGROUND" : (s->is_suspended ? "BACKGROUND" : "READY");
        solar_os_gfx_text(gfx, 260, cur_y + 16, st_str);

        char time_buf[16];
        if (s->interval_ms > 0) {
            snprintf(time_buf, sizeof(time_buf), "%ums", (unsigned)s->interval_ms);
        } else {
            strlcpy(time_buf, "-", sizeof(time_buf));
        }
        solar_os_gfx_text(gfx, 340, cur_y + 16, time_buf);

        cur_y += row_h;
    }

    /* Draw Scrollbar if needed */
    draw_scrollbar(gfx, 392, table_y + 20, (int)(visible_rows * row_h),
                   epstate.session_count, visible_rows, epstate.session_scroll_offset);
}

static void esprocess_draw_tab_tasks(solar_os_gfx_t *gfx, int top_y, int bottom_y)
{
    const int table_y = top_y + 4;
    const int row_h = 20;
    const size_t visible_rows = (size_t)((bottom_y - table_y - 20) / row_h);

    /* Header */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 6, table_y, 388, 18);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 10, table_y + 13, "TASK NAME");
    solar_os_gfx_text(gfx, 150, table_y + 13, "STATE");
    solar_os_gfx_text(gfx, 205, table_y + 13, "PRI");
    solar_os_gfx_text(gfx, 245, table_y + 13, "CPU%");
    solar_os_gfx_text(gfx, 305, table_y + 13, "FREE STACK");

    int cur_y = table_y + 20;

    const size_t start_idx = epstate.task_scroll_offset;
    const size_t end_idx = (start_idx + visible_rows < epstate.task_count) ?
                           (start_idx + visible_rows) : epstate.task_count;

    for (size_t i = start_idx; i < end_idx; i++) {
        const esprocess_task_row_t *t = &epstate.tasks[i];
        const bool is_sel = (i == epstate.selected_task);

        if (is_sel) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, 6, cur_y, 382, row_h - 2);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 10, cur_y + 14, t->name);

        char sbuf[4];
        snprintf(sbuf, sizeof(sbuf), "%c", t->state_char);
        solar_os_gfx_text(gfx, 160, cur_y + 14, sbuf);

        char pbuf[8];
        snprintf(pbuf, sizeof(pbuf), "%u", (unsigned)t->priority);
        solar_os_gfx_text(gfx, 210, cur_y + 14, pbuf);

        char cpubuf[16];
        snprintf(cpubuf, sizeof(cpubuf), "%u.%u%%", (unsigned)(t->cpu_tenths / 10), (unsigned)(t->cpu_tenths % 10));
        solar_os_gfx_text(gfx, 245, cur_y + 14, cpubuf);

        char stkbuf[16];
        format_bytes(t->stack_free_bytes, stkbuf, sizeof(stkbuf));
        solar_os_gfx_text(gfx, 305, cur_y + 14, stkbuf);

        cur_y += row_h;
    }

    /* Draw Scrollbar */
    draw_scrollbar(gfx, 392, table_y + 20, (int)(visible_rows * row_h),
                   epstate.task_count, visible_rows, epstate.task_scroll_offset);
}

static void esprocess_draw_tab_memory(solar_os_gfx_t *gfx, int top_y, int bottom_y)
{
    solar_os_memory_status_t mem_status;
    solar_os_memory_get_status(&mem_status);

    int cur_y = top_y + 8;

    /* 1. Internal SRAM Card */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 10, cur_y, 380, 56);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 18, cur_y + 16, "Internal Memory (Internal SRAM - 320 KB)");

    const size_t sram_total = mem_status.internal.total > 0 ? mem_status.internal.total : (320 * 1024);
    const size_t sram_free = mem_status.internal.free;
    const size_t sram_used = sram_total > sram_free ? sram_total - sram_free : 0;
    const int sram_pct = sram_total > 0 ? (int)((sram_used * 100) / sram_total) : 0;

    draw_progress_bar(gfx, 18, cur_y + 24, 200, 14, sram_pct);

    char sram_detail[64];
    char sram_used_str[16], sram_tot_str[16], sram_free_str[16];
    format_bytes(sram_used, sram_used_str, sizeof(sram_used_str));
    format_bytes(sram_total, sram_tot_str, sizeof(sram_tot_str));
    format_bytes(sram_free, sram_free_str, sizeof(sram_free_str));
    snprintf(sram_detail, sizeof(sram_detail), "Used: %s / %s (%d%%)  |  Free: %s",
             sram_used_str, sram_tot_str, sram_pct, sram_free_str);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 18, cur_y + 48, sram_detail);

    cur_y += 62;

    /* 2. External PSRAM Card */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 10, cur_y, 380, 56);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 18, cur_y + 16, "External Memory (Octal PSRAM - 8 MB)");

    const size_t psram_total = mem_status.external.total > 0 ? mem_status.external.total : (8 * 1024 * 1024);
    const size_t psram_free = mem_status.external.free;
    const size_t psram_used = psram_total > psram_free ? psram_total - psram_free : 0;
    const int psram_pct = psram_total > 0 ? (int)((psram_used * 100) / psram_total) : 0;

    draw_progress_bar(gfx, 18, cur_y + 24, 200, 14, psram_pct);

    char psram_detail[64];
    char psram_used_str[16], psram_tot_str[16], psram_free_str[16];
    format_bytes(psram_used, psram_used_str, sizeof(psram_used_str));
    format_bytes(psram_total, psram_tot_str, sizeof(psram_tot_str));
    format_bytes(psram_free, psram_free_str, sizeof(psram_free_str));
    snprintf(psram_detail, sizeof(psram_detail), "Used: %s / %s (%d%%)  |  Free: %s",
             psram_used_str, psram_tot_str, psram_pct, psram_free_str);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 18, cur_y + 48, psram_detail);

    cur_y += 62;

    /* 3. System Hardware & CPU Stats Card */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 10, cur_y, 380, 80);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 18, cur_y + 16, "Processor & System Information (ESP32-S3)");

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 18, cur_y + 34, "CPU: Xtensa LX7 Dual-Core @ 240 MHz (Core 0: OS / Core 1: App)");

    const uint32_t uptime_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    const uint32_t uptime_min = uptime_sec / 60;
    const uint32_t uptime_hr = uptime_min / 60;

    char sys_info[96];
    snprintf(sys_info, sizeof(sys_info),
             "Uptime: %02u:%02u:%02u  |  Tasks: %u  |  Processes: %u",
             (unsigned)uptime_hr, (unsigned)(uptime_min % 60), (unsigned)(uptime_sec % 60),
             (unsigned)epstate.task_count, (unsigned)epstate.session_count);
    solar_os_gfx_text(gfx, 18, cur_y + 52, sys_info);

    char dma_info[96];
    char dma_free_str[16];
    format_bytes(mem_status.dma.free, dma_free_str, sizeof(dma_free_str));
    snprintf(dma_info, sizeof(dma_info),
             "DMA Free: %s  |  SRAM Max Block: %u KB",
             dma_free_str, (unsigned)(mem_status.internal.largest_free / 1024));
    solar_os_gfx_text(gfx, 18, cur_y + 70, dma_info);
}

static void esprocess_draw_tab_jobs(solar_os_gfx_t *gfx, int top_y, int bottom_y)
{
    const int table_y = top_y + 4;
    const int row_h = 22;
    const size_t visible_rows = (size_t)((bottom_y - table_y - 20) / row_h);

    /* Header */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 6, table_y, 388, 18);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 10, table_y + 13, "SERVICE / JOB");
    solar_os_gfx_text(gfx, 130, table_y + 13, "STATE");
    solar_os_gfx_text(gfx, 210, table_y + 13, "TICKS");
    solar_os_gfx_text(gfx, 270, table_y + 13, "STACK");
    solar_os_gfx_text(gfx, 330, table_y + 13, "OWNER");

    int cur_y = table_y + 20;

    const size_t start_idx = epstate.job_scroll_offset;
    const size_t end_idx = (start_idx + visible_rows < epstate.job_count) ?
                           (start_idx + visible_rows) : epstate.job_count;

    for (size_t i = start_idx; i < end_idx; i++) {
        const solar_os_job_status_t *j = &epstate.jobs[i];
        const bool is_sel = (i == epstate.selected_job);

        if (is_sel) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, 6, cur_y, 382, row_h - 2);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_line(gfx, 6, cur_y + row_h - 2, 388, cur_y + row_h - 2);
        }

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 10, cur_y + 15, j->name ? j->name : "?");

        const char *st_name = (j->state == SOLAR_OS_JOB_RUNNING) ? "RUNNING" :
                              (j->state == SOLAR_OS_JOB_WAITING) ? "WAITING" : "STOPPED";
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, 130, cur_y + 15, st_name);

        char tick_buf[16];
        snprintf(tick_buf, sizeof(tick_buf), "%u", (unsigned)j->tick_count);
        solar_os_gfx_text(gfx, 210, cur_y + 15, tick_buf);

        char stk_buf[16];
        snprintf(stk_buf, sizeof(stk_buf), "%u B", (unsigned)j->worker_stack_bytes);
        solar_os_gfx_text(gfx, 270, cur_y + 15, stk_buf);

        char owner_buf[16];
        strlcpy(owner_buf, j->owner[0] ? j->owner : "system", sizeof(owner_buf));
        solar_os_gfx_text(gfx, 330, cur_y + 15, owner_buf);

        cur_y += row_h;
    }

    /* Draw Scrollbar */
    draw_scrollbar(gfx, 392, table_y + 20, (int)(visible_rows * row_h),
                   epstate.job_count, visible_rows, epstate.job_scroll_offset);
}

static void draw_help_modal(solar_os_gfx_t *gfx)
{
    const int hx = 16;
    const int hy = 20;
    const int hw = 368;
    const int hh = 256;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, hx, hy, hw, hh);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, hx, hy, hw, hh);
    solar_os_gfx_rect(gfx, hx + 1, hy + 1, hw - 2, hh - 2);

    /* Title */
    solar_os_gfx_line(gfx, hx, hy + 24, hx + hw, hy + 24);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, hx + 12, hy + 18, "Process & System Manager Help");

    int text_y = hy + 42;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, hx + 14, text_y, "Navigation & Shortcuts:");

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    text_y += 18;
    solar_os_gfx_text(gfx, hx + 14, text_y, "- [1-4] or [Tab]: Switch tabs (Processes / Tasks / Mem / Services)");
    text_y += 16;
    solar_os_gfx_text(gfx, hx + 14, text_y, "- [Up/Down] / [Wheel]: Select and scroll items");
    text_y += 16;
    solar_os_gfx_text(gfx, hx + 14, text_y, "- [G] or [Enter]: Bring selected process to foreground");
    text_y += 16;
    solar_os_gfx_text(gfx, hx + 14, text_y, "- [D] / [Del]: Terminate process OR toggle background service");
    text_y += 16;
    solar_os_gfx_text(gfx, hx + 14, text_y, "- [R] / [Y]: Force refresh data snapshot");
    text_y += 16;
    solar_os_gfx_text(gfx, hx + 14, text_y, "- [?] / [H]: Toggle this help overlay");
    text_y += 16;
    solar_os_gfx_text(gfx, hx + 14, text_y, "- [ESC] / [Q]: Exit application");

    text_y += 22;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, hx + 14, text_y, "Task States:");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    text_y += 16;
    solar_os_gfx_text(gfx, hx + 14, text_y, "R = Running  |  r = Ready  |  B = Blocked  |  S = Suspended");

    /* Footer dismiss prompt */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, hx, hy + hh - 24, hx + hw, hy + hh - 24);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, hx + 40, hy + hh - 8, "Press [ESC], [Enter], [?] or click to close");
}

static void esprocess_render(solar_os_context_t *ctx)
{
    if (ctx == NULL) return;
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Draw AppBar with Tabs */
    const solar_os_appbar_header_t header = {
        .title = "esprocess",
        .show_back = true,
        .tabs = {
            .names = tab_names,
            .count = ESPROCESS_TAB_COUNT,
            .active_index = (size_t)epstate.tab,
        },
    };
    solar_os_appbar_draw_header(gfx, &header);

    const int top_y = 34;
    const int bottom_y = 265;

    /* 2. Draw Tab Content */
    switch (epstate.tab) {
    case ESPROCESS_TAB_SESSIONS:
        esprocess_draw_tab_sessions(gfx, top_y, bottom_y);
        break;
    case ESPROCESS_TAB_TASKS:
        esprocess_draw_tab_tasks(gfx, top_y, bottom_y);
        break;
    case ESPROCESS_TAB_MEMORY:
        esprocess_draw_tab_memory(gfx, top_y, bottom_y);
        break;
    case ESPROCESS_TAB_JOBS:
        esprocess_draw_tab_jobs(gfx, top_y, bottom_y);
        break;
    }

    /* 3. Draw Notice / Status Banner */
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
    if (epstate.notice_msg[0] != '\0' && now < epstate.notice_until_ms) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_fill_rect(gfx, 10, bottom_y - 20, 380, 20);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 16, bottom_y - 6, epstate.notice_msg);
    }

    /* 4. Draw Footer Action Bar */
    solar_os_appbar_shortcut_t shortcuts[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    size_t sc_count = 0;

    if (epstate.tab == ESPROCESS_TAB_SESSIONS) {
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = 'G', .label = "Focus" };
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = 'D', .label = "Terminate" };
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = 'R', .label = "Refresh" };
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = '?', .label = "Help" };
    } else if (epstate.tab == ESPROCESS_TAB_TASKS) {
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = 'R', .label = "Refresh" };
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = '?', .label = "Help" };
    } else if (epstate.tab == ESPROCESS_TAB_JOBS) {
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = 'D', .label = "Start/Stop" };
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = 'R', .label = "Refresh" };
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = '?', .label = "Help" };
    } else {
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = 'R', .label = "Refresh" };
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = '?', .label = "Help" };
    }

    const solar_os_appbar_shortcuts_t footer = {
        .items = shortcuts,
        .count = sc_count,
    };
    solar_os_appbar_draw_footer(gfx, &footer);

    /* 5. Draw Help Overlay if toggled */
    if (epstate.show_help) {
        draw_help_modal(gfx);
    }

    solar_os_gfx_present(gfx);
}

static void set_notice(const char *msg)
{
    if (msg != NULL) {
        strlcpy(epstate.notice_msg, msg, sizeof(epstate.notice_msg));
        epstate.notice_until_ms = (uint32_t)(esp_timer_get_time() / 1000U) + 3000U;
    }
}

static void switch_to_selected_session(void)
{
    if (epstate.selected_session < epstate.session_count) {
        const uint8_t sid = epstate.sessions[epstate.selected_session].id;
        set_notice("Switching to session...");
        if (!solar_os_sessions_switch_to_session_id(sid)) {
            set_notice("Could not switch to session!");
        }
    }
}

static void terminate_selected_session(void)
{
    if (epstate.selected_session < epstate.session_count) {
        const uint8_t sid = epstate.sessions[epstate.selected_session].id;
        const char *name = epstate.sessions[epstate.selected_session].app_name;
        char nbuf[64];
        snprintf(nbuf, sizeof(nbuf), "#%u (%s) terminated.", (unsigned)sid, name);

        if (solar_os_sessions_terminate_session_id(sid) == ESP_OK) {
            set_notice(nbuf);
            esprocess_refresh_data();
        } else {
            set_notice("Failed to terminate!");
        }
    }
}

static esp_err_t esprocess_start(solar_os_context_t *ctx)
{
    if (ctx == NULL) return ESP_ERR_INVALID_ARG;
    epstate.ctx = ctx;
    epstate.tab = ESPROCESS_TAB_SESSIONS;
    epstate.selected_session = 0;
    epstate.selected_task = 0;
    epstate.selected_job = 0;
    epstate.session_scroll_offset = 0;
    epstate.task_scroll_offset = 0;
    epstate.job_scroll_offset = 0;
    epstate.notice_msg[0] = '\0';
    epstate.notice_until_ms = 0;
    epstate.show_help = false;

    solar_os_context_set_graphics_active(ctx, true);
    esprocess_refresh_data();
    esprocess_render(ctx);
    return ESP_OK;
}

static void esprocess_stop(solar_os_context_t *ctx)
{
    if (ctx != NULL) {
        solar_os_context_set_graphics_active(ctx, false);
    }
}

static bool esprocess_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (ctx == NULL || event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_RESUME) {
        solar_os_context_set_graphics_active(ctx, true);
        esprocess_refresh_data();
        esprocess_render(ctx);
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
        /* Auto-refresh quietly every 5 seconds or when notice banner expires */
        if (now - epstate.last_refresh_ms >= ESPROCESS_AUTO_REFRESH_MS ||
            (epstate.notice_until_ms != 0 && now >= epstate.notice_until_ms)) {
            if (epstate.notice_until_ms != 0 && now >= epstate.notice_until_ms) {
                epstate.notice_msg[0] = '\0';
                epstate.notice_until_ms = 0;
            }
            esprocess_refresh_data();
            esprocess_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_SCROLL) {
        if (epstate.show_help) {
            return true;
        }
        const int delta = (event->data.scroll.delta < 0) ? 1 : -1;
        if (epstate.tab == ESPROCESS_TAB_SESSIONS) {
            adjust_scroll(&epstate.selected_session, &epstate.session_scroll_offset,
                          epstate.session_count, 9, delta);
        } else if (epstate.tab == ESPROCESS_TAB_TASKS) {
            adjust_scroll(&epstate.selected_task, &epstate.task_scroll_offset,
                          epstate.task_count, 10, delta);
        } else if (epstate.tab == ESPROCESS_TAB_JOBS) {
            adjust_scroll(&epstate.selected_job, &epstate.job_scroll_offset,
                          epstate.job_count, 9, delta);
        }
        esprocess_render(ctx);
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return false;

        if (epstate.show_help) {
            epstate.show_help = false;
            esprocess_render(ctx);
            return true;
        }

        const int px = event->data.click.x;
        const int py = event->data.click.y;

        /* Check AppBar click (Back or Tabs) */
        const solar_os_appbar_header_t header = {
            .title = "esprocess",
            .show_back = true,
            .tabs = {
                .names = tab_names,
                .count = ESPROCESS_TAB_COUNT,
                .active_index = (size_t)epstate.tab,
            },
        };
        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, px, py, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                solar_os_context_request_exit(ctx);
                return true;
            } else if (hit.kind == SOLAR_OS_APPBAR_HIT_TAB_ITEM && hit.index < ESPROCESS_TAB_COUNT) {
                epstate.tab = (esprocess_tab_t)hit.index;
                esprocess_render(ctx);
                return true;
            }
        }

        /* Check List Rows click */
        if (py >= 58 && py <= 255) {
            if (epstate.tab == ESPROCESS_TAB_SESSIONS) {
                const size_t visible_rows = 9;
                const size_t row_idx = (size_t)((py - 58) / 24) + epstate.session_scroll_offset;
                if (row_idx < epstate.session_count) {
                    if (epstate.selected_session == row_idx) {
                        switch_to_selected_session();
                    } else {
                        epstate.selected_session = row_idx;
                        adjust_scroll(&epstate.selected_session, &epstate.session_scroll_offset,
                                      epstate.session_count, visible_rows, 0);
                        esprocess_render(ctx);
                    }
                    return true;
                }
            } else if (epstate.tab == ESPROCESS_TAB_TASKS) {
                const size_t visible_rows = 10;
                const size_t row_idx = (size_t)((py - 58) / 20) + epstate.task_scroll_offset;
                if (row_idx < epstate.task_count) {
                    epstate.selected_task = row_idx;
                    adjust_scroll(&epstate.selected_task, &epstate.task_scroll_offset,
                                  epstate.task_count, visible_rows, 0);
                    esprocess_render(ctx);
                    return true;
                }
            } else if (epstate.tab == ESPROCESS_TAB_JOBS) {
                const size_t visible_rows = 9;
                const size_t row_idx = (size_t)((py - 58) / 22) + epstate.job_scroll_offset;
                if (row_idx < epstate.job_count) {
                    epstate.selected_job = row_idx;
                    adjust_scroll(&epstate.selected_job, &epstate.job_scroll_offset,
                                  epstate.job_count, visible_rows, 0);
                    esprocess_render(ctx);
                    return true;
                }
            }
        }

        /* Check Footer buttons hit-test */
        solar_os_appbar_shortcut_t shortcuts_click[SOLAR_OS_APPBAR_SHORTCUT_MAX];
        size_t sc_c = 0;
        if (epstate.tab == ESPROCESS_TAB_SESSIONS) {
            shortcuts_click[sc_c++] = (solar_os_appbar_shortcut_t){ .key = 'G', .label = "Focus" };
            shortcuts_click[sc_c++] = (solar_os_appbar_shortcut_t){ .key = 'D', .label = "Terminate" };
            shortcuts_click[sc_c++] = (solar_os_appbar_shortcut_t){ .key = 'R', .label = "Refresh" };
            shortcuts_click[sc_c++] = (solar_os_appbar_shortcut_t){ .key = '?', .label = "Help" };
        } else if (epstate.tab == ESPROCESS_TAB_TASKS) {
            shortcuts_click[sc_c++] = (solar_os_appbar_shortcut_t){ .key = 'R', .label = "Refresh" };
            shortcuts_click[sc_c++] = (solar_os_appbar_shortcut_t){ .key = '?', .label = "Help" };
        } else if (epstate.tab == ESPROCESS_TAB_JOBS) {
            shortcuts_click[sc_c++] = (solar_os_appbar_shortcut_t){ .key = 'D', .label = "Start/Stop" };
            shortcuts_click[sc_c++] = (solar_os_appbar_shortcut_t){ .key = 'R', .label = "Refresh" };
            shortcuts_click[sc_c++] = (solar_os_appbar_shortcut_t){ .key = '?', .label = "Help" };
        } else {
            shortcuts_click[sc_c++] = (solar_os_appbar_shortcut_t){ .key = 'R', .label = "Refresh" };
            shortcuts_click[sc_c++] = (solar_os_appbar_shortcut_t){ .key = '?', .label = "Help" };
        }
        const solar_os_appbar_shortcuts_t footer_click = {
            .items = shortcuts_click,
            .count = sc_c,
        };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &footer_click, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM && fhit.index < sc_c) {
                const char k = shortcuts_click[fhit.index].key;
                if (k == 'G') {
                    switch_to_selected_session();
                } else if (k == 'D') {
                    if (epstate.tab == ESPROCESS_TAB_SESSIONS) {
                        terminate_selected_session();
                    } else if (epstate.tab == ESPROCESS_TAB_JOBS && epstate.selected_job < epstate.job_count) {
                        const char *jname = epstate.jobs[epstate.selected_job].name;
                        if (epstate.jobs[epstate.selected_job].state == SOLAR_OS_JOB_RUNNING) {
                            solar_os_jobs_stop(ctx, jname);
                            set_notice("Service stopped.");
                        } else {
                            solar_os_jobs_start(ctx, jname, 0, NULL);
                            set_notice("Service started.");
                        }
                        esprocess_refresh_data();
                    }
                    esprocess_render(ctx);
                } else if (k == 'R' || k == 'Y') {
                    esprocess_refresh_data();
                    set_notice("Refreshed.");
                    esprocess_render(ctx);
                } else if (k == '?' || k == 'H') {
                    epstate.show_help = !epstate.show_help;
                    esprocess_render(ctx);
                }
                return true;
            }
        }
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (epstate.show_help) {
            if (ch == SOLAR_OS_KEY_ESCAPE || ch == '\r' || ch == '\n' ||
                ch == '?' || ch == 'h' || ch == 'H' || ch == 'q' || ch == 'Q') {
                epstate.show_help = false;
                esprocess_render(ctx);
                return true;
            }
            return true;
        }

        if (ch >= '1' && ch <= '4') {
            epstate.tab = (esprocess_tab_t)(ch - '1');
            esprocess_render(ctx);
            return true;
        }

        if (ch == '\t') {
            epstate.tab = (esprocess_tab_t)((epstate.tab + 1) % ESPROCESS_TAB_COUNT);
            esprocess_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_UP || ch == 'k') {
            if (epstate.tab == ESPROCESS_TAB_SESSIONS) {
                adjust_scroll(&epstate.selected_session, &epstate.session_scroll_offset,
                              epstate.session_count, 9, -1);
            } else if (epstate.tab == ESPROCESS_TAB_TASKS) {
                adjust_scroll(&epstate.selected_task, &epstate.task_scroll_offset,
                              epstate.task_count, 10, -1);
            } else if (epstate.tab == ESPROCESS_TAB_JOBS) {
                adjust_scroll(&epstate.selected_job, &epstate.job_scroll_offset,
                              epstate.job_count, 9, -1);
            }
            esprocess_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_DOWN || ch == 'j') {
            if (epstate.tab == ESPROCESS_TAB_SESSIONS) {
                adjust_scroll(&epstate.selected_session, &epstate.session_scroll_offset,
                              epstate.session_count, 9, 1);
            } else if (epstate.tab == ESPROCESS_TAB_TASKS) {
                adjust_scroll(&epstate.selected_task, &epstate.task_scroll_offset,
                              epstate.task_count, 10, 1);
            } else if (epstate.tab == ESPROCESS_TAB_JOBS) {
                adjust_scroll(&epstate.selected_job, &epstate.job_scroll_offset,
                              epstate.job_count, 9, 1);
            }
            esprocess_render(ctx);
            return true;
        }

        if (ch == '\r' || ch == '\n' || ch == 'g' || ch == 'G') {
            if (epstate.tab == ESPROCESS_TAB_SESSIONS) {
                switch_to_selected_session();
                return true;
            }
        }

        if (ch == 'd' || ch == 'D' || ch == SOLAR_OS_KEY_DELETE || ch == 0x7f) {
            if (epstate.tab == ESPROCESS_TAB_SESSIONS) {
                terminate_selected_session();
                esprocess_render(ctx);
                return true;
            } else if (epstate.tab == ESPROCESS_TAB_JOBS && epstate.selected_job < epstate.job_count) {
                const char *jname = epstate.jobs[epstate.selected_job].name;
                if (epstate.jobs[epstate.selected_job].state == SOLAR_OS_JOB_RUNNING) {
                    solar_os_jobs_stop(ctx, jname);
                    set_notice("Service stopped.");
                } else {
                    solar_os_jobs_start(ctx, jname, 0, NULL);
                    set_notice("Service started.");
                }
                esprocess_refresh_data();
                esprocess_render(ctx);
                return true;
            }
        }

        if (ch == 'r' || ch == 'R' || ch == 'y' || ch == 'Y') {
            esprocess_refresh_data();
            set_notice("Refreshed.");
            esprocess_render(ctx);
            return true;
        }

        if (ch == '?' || ch == 'h' || ch == 'H') {
            epstate.show_help = !epstate.show_help;
            esprocess_render(ctx);
            return true;
        }

        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_esprocess_app = {
    .name = "esprocess",
    .summary = "task, process, cpu and memory monitor and manager",
    .flags = 0,
    .start = esprocess_start,
    .stop = esprocess_stop,
    .event = esprocess_event,
    .state_slot = &esprocess_state_ptr,
    .state_size = sizeof(esprocess_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = 0,
};
