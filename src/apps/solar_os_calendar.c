#include "solar_os_calendar.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "solar_os.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"
#include "solar_os_time.h"

#define CALENDAR_STACK_SIZE 4096
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(CALENDAR_STACK_SIZE);

typedef struct {
    int view_year;
    int view_month; /* 1..12 */
    int current_year;
    int current_month;
    int current_day;
    uint32_t last_tick_ms;
} calendar_state_t;

static void *calendar_state_ptr;
#define cal (*(calendar_state_t *)calendar_state_ptr)

static const char *const month_names[] = {
    "", "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static const char *const weekday_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static bool is_leap_year(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m)
{
    static const int d[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && is_leap_year(y)) return 29;
    if (m >= 1 && m <= 12) return d[m];
    return 30;
}

/* Zeller's congruence for day of week: 0=Sun, 1=Mon, ..., 6=Sat */
static int day_of_week(int y, int m, int d)
{
    struct tm t = {
        .tm_year = y - 1900,
        .tm_mon = m - 1,
        .tm_mday = d,
    };
    time_t raw = mktime(&t);
    if (raw == (time_t)-1) return 0;
    struct tm *lt = localtime(&raw);
    return lt != NULL ? lt->tm_wday : 0;
}

static void cal_update_current_time(void)
{
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (lt != NULL && lt->tm_year > 100) {
        cal.current_year = lt->tm_year + 1900;
        cal.current_month = lt->tm_mon + 1;
        cal.current_day = lt->tm_mday;
    } else {
        cal.current_year = 2026;
        cal.current_month = 8;
        cal.current_day = 14;
    }
}

static void cal_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Header Bar */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, screen_w, 24);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 8, 16, "CALENDAR & LIVE CLOCK");

    /* 2. Left Side: Monthly Calendar Grid (X: 12..210, Y: 32..265) */
    char cal_title[32];
    snprintf(cal_title, sizeof(cal_title), "%s %d", month_names[cal.view_month], cal.view_year);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 16, 44, cal_title);

    /* Weekday Headers */
    const int cell_w = 26;
    const int cell_h = 24;
    const int grid_x = 12;
    const int grid_y = 52;

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    for (int w = 0; w < 7; w++) {
        solar_os_gfx_text(gfx, grid_x + w * cell_w + 3, grid_y + 12, weekday_names[w]);
    }
    solar_os_gfx_line(gfx, grid_x, grid_y + 16, grid_x + 7 * cell_w, grid_y + 16);

    /* Days */
    const int first_dow = day_of_week(cal.view_year, cal.view_month, 1);
    const int total_days = days_in_month(cal.view_year, cal.view_month);

    int row = 0;
    int col = first_dow;

    for (int day = 1; day <= total_days; day++) {
        const int dx = grid_x + col * cell_w;
        const int dy = grid_y + 20 + row * cell_h;

        const bool is_today = (cal.view_year == cal.current_year &&
                               cal.view_month == cal.current_month &&
                               day == cal.current_day);

        if (is_today) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, dx, dy, cell_w - 2, cell_h - 2);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        }

        char day_str[8];
        snprintf(day_str, sizeof(day_str), "%d", day);
        solar_os_gfx_set_font(gfx, is_today ? SOLAR_OS_GFX_FONT_BOLD : SOLAR_OS_GFX_FONT_SMALL);
        const size_t dw = solar_os_gfx_text_width(gfx, day_str);
        solar_os_gfx_text(gfx, dx + (cell_w - 2 - (int)dw) / 2, dy + 15, day_str);

        col++;
        if (col >= 7) {
            col = 0;
            row++;
        }
    }

    /* Vertical Separator */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, 215, 30, 215, 270);

    /* 3. Right Side: Analog & Digital Clock (X: 225..390) */
    time_t raw_time = time(NULL);
    struct tm *t = localtime(&raw_time);
    const int hr = t != NULL ? t->tm_hour : 12;
    const int min = t != NULL ? t->tm_min : 0;
    const int sec = t != NULL ? t->tm_sec : 0;

    /* Analog Clock Dial (Center: 305, 110, Radius: 50) */
    const int acx = 305;
    const int acy = 100;
    const int arad = 48;

    solar_os_gfx_circle(gfx, acx, acy, arad);
    solar_os_gfx_circle(gfx, acx, acy, arad - 1);
    solar_os_gfx_fill_circle(gfx, acx, acy, 3);

    /* Hour Marks (12 ticks) */
    for (int h = 0; h < 12; h++) {
        const float a = ((float)h / 12.0f) * 2.0f * (float)M_PI;
        const int x0 = acx + (int)((float)(arad - 6) * sinf(a));
        const int y0 = acy - (int)((float)(arad - 6) * cosf(a));
        const int x1 = acx + (int)((float)arad * sinf(a));
        const int y1 = acy - (int)((float)arad * cosf(a));
        solar_os_gfx_line(gfx, x0, y0, x1, y1);
    }

    /* Hour Hand */
    const float hr_angle = ((float)(hr % 12) + (float)min / 60.0f) / 12.0f * 2.0f * (float)M_PI;
    const int hx = acx + (int)(26.0f * sinf(hr_angle));
    const int hy = acy - (int)(26.0f * cosf(hr_angle));
    solar_os_gfx_line(gfx, acx, acy, hx, hy);
    solar_os_gfx_line(gfx, acx + 1, acy, hx + 1, hy);

    /* Minute Hand */
    const float min_angle = ((float)min + (float)sec / 60.0f) / 60.0f * 2.0f * (float)M_PI;
    const int mx = acx + (int)(38.0f * sinf(min_angle));
    const int my = acy - (int)(38.0f * cosf(min_angle));
    solar_os_gfx_line(gfx, acx, acy, mx, my);

    /* Second Hand */
    const float sec_angle = (float)sec / 60.0f * 2.0f * (float)M_PI;
    const int sx = acx + (int)(42.0f * sinf(sec_angle));
    const int sy = acy - (int)(42.0f * cosf(sec_angle));
    solar_os_gfx_line(gfx, acx, acy, sx, sy);

    /* Big Digital Clock beneath analog dial */
    char digi_str[32];
    snprintf(digi_str, sizeof(digi_str), "%02d:%02d:%02d", hr, min, sec);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_18);
    const size_t d_w = solar_os_gfx_text_width(gfx, digi_str);
    solar_os_gfx_text(gfx, acx - (int)d_w / 2, 185, digi_str);

    /* Full Date Text */
    char date_full[48];
    snprintf(date_full, sizeof(date_full), "%02d %s %d", cal.current_day, month_names[cal.current_month], cal.current_year);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const size_t df_w = solar_os_gfx_text_width(gfx, date_full);
    solar_os_gfx_text(gfx, acx - (int)df_w / 2, 210, date_full);

    /* 4. Footer Bar */
    solar_os_gfx_fill_rect(gfx, 0, 278, screen_w, 22);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 8, 293, "[LEFT/RIGHT] Prev/Next Month | [T] Today | [ESC] Exit");

    solar_os_gfx_present(gfx);
}

static esp_err_t cal_start(solar_os_context_t *ctx)
{
    cal_update_current_time();
    cal.view_year = cal.current_year;
    cal.view_month = cal.current_month;
    cal.last_tick_ms = (uint32_t)(esp_timer_get_time() / 1000U);

    solar_os_context_set_graphics_active(ctx, true);
    cal_render(ctx);
    return ESP_OK;
}

static void cal_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool cal_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);
        if (now - cal.last_tick_ms >= 1000U) {
            cal.last_tick_ms = now;
            cal_update_current_time();
            cal_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;
        if (ch == SOLAR_OS_KEY_LEFT || ch == 'a' || ch == 'A' || ch == 'h' || ch == 'H') {
            if (cal.view_month > 1) {
                cal.view_month--;
            } else {
                cal.view_month = 12;
                cal.view_year--;
            }
            cal_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_RIGHT || ch == 'd' || ch == 'D' || ch == 'l' || ch == 'L') {
            if (cal.view_month < 12) {
                cal.view_month++;
            } else {
                cal.view_month = 1;
                cal.view_year++;
            }
            cal_render(ctx);
            return true;
        }
        if (ch == 't' || ch == 'T') {
            cal_update_current_time();
            cal.view_year = cal.current_year;
            cal.view_month = cal.current_month;
            cal_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_calendar_app = {
    .name = "calendar",
    .summary = "monthly calendar and live clock",
    .flags = 0,
    .start = cal_start,
    .stop = cal_stop,
    .event = cal_event,
    .state_slot = &calendar_state_ptr,
    .state_size = sizeof(calendar_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = CALENDAR_STACK_SIZE,
};
