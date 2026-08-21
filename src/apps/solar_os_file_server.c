#include "solar_os_file_server.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "nvs.h"
#include "solar_os.h"
#include "solar_os_agent.h"
#include "solar_os_gfx.h"
#include "solar_os_http_server.h"
#include "solar_os_keys.h"
#include "solar_os_resource_limits.h"
#include "solar_os_storage.h"
#include "solar_os_time.h"
#include "solar_os_wifi.h"
#include "solar_os_appbar.h"
#include "solar_os_help.h"
#include "solar_os_log.h"

#define FILE_SERVER_STACK_SIZE 8192
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(FILE_SERVER_STACK_SIZE);

typedef struct {
    bool running;
    char ip_str[32];
    uint16_t port;
    uint32_t uploads_count;
    uint32_t downloads_count;
    char last_file[64];
    uint32_t last_tick_ms;
    bool show_help;
} file_server_state_t;

static const char *const fserver_help_lines[] = {
    "Shares the SD card over Wi-Fi as a web page.",
    "",
    "  - Open the shown http://<ip>/ address in any",
    "    phone or PC browser on the same network.",
    "  - From the browser you can upload, download,",
    "    rename, move and delete files and folders,",
    "    and edit device settings.",
    "",
    "Refresh re-reads the IP. Exiting the app keeps the",
    "server running in the background.",
};
#define FSERVER_HELP_LINE_COUNT (sizeof(fserver_help_lines) / sizeof(fserver_help_lines[0]))

static size_t fserver_build_footer(solar_os_appbar_shortcut_t *items, size_t max)
{
    size_t n = 0;
    if (n < max) { items[n].key = 'r'; items[n].ctrl = false;
        snprintf(items[n].label, sizeof(items[n].label), "Refresh"); n++; }
    if (n < max) { solar_os_help_chip(&items[n]); n++; }
    return n;
}

static void *file_server_state_ptr;
#define fserver (*(file_server_state_t *)file_server_state_ptr)

static const char *get_storage_mount(void)
{
    if (solar_os_storage_sd_is_mounted()) {
        const char *m = solar_os_storage_sd_mount_point();
        if (m != NULL && m[0] != '\0') return m;
    }
    if (solar_os_storage_flash_is_mounted()) {
        const char *m = solar_os_storage_flash_mount_point();
        if (m != NULL && m[0] != '\0') return m;
    }
    return "/sdcard";
}

static void escape_html_str(const char *src, char *dst, size_t dst_len)
{
    if (src == NULL || dst == NULL || dst_len == 0) return;
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0' && d + 6 < dst_len; s++) {
        unsigned char c = (unsigned char)src[s];
        if (c == '<') {
            memcpy(&dst[d], "&lt;", 4); d += 4;
        } else if (c == '>') {
            memcpy(&dst[d], "&gt;", 4); d += 4;
        } else if (c == '&') {
            memcpy(&dst[d], "&amp;", 5); d += 5;
        } else if (c == '"') {
            memcpy(&dst[d], "&quot;", 6); d += 6;
        } else if (c == '\'') {
            memcpy(&dst[d], "&#39;", 5); d += 5;
        } else if (c < 32 || c == 127) {
            dst[d++] = '?';
        } else {
            dst[d++] = (char)c;
        }
    }
    dst[d] = '\0';
}

static void url_encode_str(const char *src, char *dst, size_t dst_len)
{
    if (src == NULL || dst == NULL || dst_len == 0) return;
    size_t d = 0;
    static const char hex[] = "0123456789ABCDEF";
    for (size_t s = 0; src[s] != '\0' && d + 4 < dst_len; s++) {
        unsigned char c = (unsigned char)src[s];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            dst[d++] = (char)c;
        } else {
            dst[d++] = '%';
            dst[d++] = hex[(c >> 4) & 0xF];
            dst[d++] = hex[c & 0xF];
        }
    }
    dst[d] = '\0';
}

static void url_decode_inplace(char *str)
{
    char *p = str;
    char *q = str;
    while (*p) {
        if (*p == '%' && p[1] && p[2] && isxdigit((int)p[1]) && isxdigit((int)p[2])) {
            char hex[3] = {p[1], p[2], '\0'};
            *q++ = (char)strtol(hex, NULL, 16);
            p += 3;
        } else if (*p == '+') {
            *q++ = ' ';
            p++;
        } else {
            *q++ = *p++;
        }
    }
    *q = '\0';
}

typedef struct {
    char buf[512];
    char full_dir[192];
    char dir_esc[96];
    char dir_enc[192];
    char item_rel[192];
    char item_full[256];
    char name_esc[96];
    char url_enc[192];
    char opt[256];
    char sbuf[384];
} web_scratch_t;

static void render_directory_tree_node(httpd_req_t *req, const char *base_mount, const char *rel_dir, web_scratch_t *sc)
{
    if (base_mount == NULL || sc == NULL) return;
    if (rel_dir == NULL || rel_dir[0] == '\0') {
        snprintf(sc->full_dir, sizeof(sc->full_dir), "%s", base_mount);
    } else {
        snprintf(sc->full_dir, sizeof(sc->full_dir), "%s/%s", base_mount, rel_dir);
    }

    DIR *d = opendir(sc->full_dir);
    if (d == NULL) return;

    escape_html_str(rel_dir && rel_dir[0] ? rel_dir : "Root (/)", sc->dir_esc, sizeof(sc->dir_esc));
    url_encode_str(rel_dir && rel_dir[0] ? rel_dir : "", sc->dir_enc, sizeof(sc->dir_enc));

    snprintf(sc->buf, sizeof(sc->buf),
             "<div class='folder-card'>"
             "<div class='folder-header'>"
             "<div><strong>📁 %s</strong></div>"
             "<div style='display:flex;gap:6px;align-items:center;'>"
             "<button class='act-btn' title='New Subfolder' onclick=\"newFolderIn('%s')\">➕</button>",
             sc->dir_esc, sc->dir_enc);
    httpd_resp_send_chunk(req, sc->buf, HTTPD_RESP_USE_STRLEN);

    if (rel_dir != NULL && rel_dir[0] != '\0') {
        snprintf(sc->buf, sizeof(sc->buf),
                 "<button class='act-btn' title='Move' onclick=\"movePath('%s',true)\">📦</button>"
                 "<button class='act-btn' title='Rename' onclick=\"renamePath('%s',true)\">✏️</button>"
                 "<button class='act-btn act-del' title='Delete' onclick=\"delFile('%s',true)\">🗑️</button>",
                 sc->dir_enc, sc->dir_enc, sc->dir_enc);
        httpd_resp_send_chunk(req, sc->buf, HTTPD_RESP_USE_STRLEN);
    }

    snprintf(sc->buf, sizeof(sc->buf),
             "</div></div>"
             "<table class='file-table'><thead><tr>"
             "<th style='width:32px;text-align:center;'><input type='checkbox' class='folder-chk-all' onchange='toggleFolderChecks(this)'></th>"
             "<th>Name</th><th style='width:85px'>Size</th><th style='width:140px'>Actions</th>"
             "</tr></thead><tbody>");
    httpd_resp_send_chunk(req, sc->buf, HTTPD_RESP_USE_STRLEN);

    struct dirent *ent;
    size_t items_in_folder = 0;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        if (rel_dir == NULL || rel_dir[0] == '\0') {
            snprintf(sc->item_rel, sizeof(sc->item_rel), "%s", ent->d_name);
        } else {
            snprintf(sc->item_rel, sizeof(sc->item_rel), "%s/%s", rel_dir, ent->d_name);
        }

        snprintf(sc->item_full, sizeof(sc->item_full), "%s/%s", base_mount, sc->item_rel);

        struct stat st;
        bool is_dir = false;
        size_t size = 0;
        if (stat(sc->item_full, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = st.st_size;
        }

        if (is_dir) continue; /* Sub-folders handled separately */

        items_in_folder++;
        escape_html_str(ent->d_name, sc->name_esc, sizeof(sc->name_esc));
        url_encode_str(sc->item_rel, sc->url_enc, sizeof(sc->url_enc));

        snprintf(sc->buf, sizeof(sc->buf),
                 "<tr><td style='text-align:center;'><input type='checkbox' class='file-chk' data-path='%s' data-name='%s' onchange='updateSelection()'></td>"
                 "<td>📄 %s</td><td>%u KB</td><td class='act-cell'>",
                 sc->url_enc, sc->name_esc, sc->name_esc, (unsigned)(size / 1024));
        httpd_resp_send_chunk(req, sc->buf, HTTPD_RESP_USE_STRLEN);

        snprintf(sc->buf, sizeof(sc->buf), "<a class='act-btn' title='Download' href='/download?file=%s'>⬇️</a>", sc->url_enc);
        httpd_resp_send_chunk(req, sc->buf, HTTPD_RESP_USE_STRLEN);

        snprintf(sc->buf, sizeof(sc->buf), "<a class='act-btn' title='Move' href='javascript:void(0)' onclick=\"movePath('%s',false)\">📦</a>", sc->url_enc);
        httpd_resp_send_chunk(req, sc->buf, HTTPD_RESP_USE_STRLEN);

        snprintf(sc->buf, sizeof(sc->buf), "<a class='act-btn' title='Rename' href='javascript:void(0)' onclick=\"renamePath('%s',false)\">✏️</a>", sc->url_enc);
        httpd_resp_send_chunk(req, sc->buf, HTTPD_RESP_USE_STRLEN);

        snprintf(sc->buf, sizeof(sc->buf), "<a class='act-btn act-del' title='Delete' href='javascript:void(0)' onclick=\"delFile('%s',false)\">🗑️</a></td></tr>", sc->url_enc);
        httpd_resp_send_chunk(req, sc->buf, HTTPD_RESP_USE_STRLEN);
    }
    closedir(d);

    if (items_in_folder == 0) {
        httpd_resp_send_chunk(req, "<tr><td colspan='4' style='color:#a0aec0;font-style:italic;'>(Empty folder)</td></tr>", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_chunk(req, "</tbody></table></div>", HTTPD_RESP_USE_STRLEN);
}

static void render_folder_option_tags(httpd_req_t *req, const char *base_mount, const char *rel_dir, web_scratch_t *sc)
{
    if (base_mount == NULL || sc == NULL) return;
    if (rel_dir == NULL || rel_dir[0] == '\0') {
        snprintf(sc->full_dir, sizeof(sc->full_dir), "%s", base_mount);
    } else {
        snprintf(sc->full_dir, sizeof(sc->full_dir), "%s/%s", base_mount, rel_dir);
    }

    DIR *d = opendir(sc->full_dir);
    if (d == NULL) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char sub_full[256];
        snprintf(sub_full, sizeof(sub_full), "%s/%s", sc->full_dir, ent->d_name);
        struct stat st;
        if (stat(sub_full, &st) == 0 && S_ISDIR(st.st_mode)) {
            char sub_rel[192];
            if (rel_dir == NULL || rel_dir[0] == '\0') {
                snprintf(sub_rel, sizeof(sub_rel), "%s", ent->d_name);
            } else {
                snprintf(sub_rel, sizeof(sub_rel), "%s/%s", rel_dir, ent->d_name);
            }
            snprintf(sc->opt, sizeof(sc->opt), "<option value='%s'>/%s</option>", sub_rel, sub_rel);
            httpd_resp_send_chunk(req, sc->opt, HTTPD_RESP_USE_STRLEN);
        }
    }
    closedir(d);
}

static esp_err_t http_handle_root(httpd_req_t *req, void *user)
{
    (void)user;
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    web_scratch_t *sc = (web_scratch_t *)malloc(sizeof(web_scratch_t));
    if (sc == NULL) {
        return httpd_resp_send_500(req);
    }

    solar_os_agent_status_t agent_st = {0};
    (void)solar_os_agent_get_status(&agent_st);

    char cur_endpoint[192] = "";
    escape_html_str(agent_st.endpoint[0] ? agent_st.endpoint : "https://api.openai.com/v1/chat/completions", cur_endpoint, sizeof(cur_endpoint));

    char cur_model[64] = "";
    escape_html_str(agent_st.model[0] ? agent_st.model : "gpt-4o-mini", cur_model, sizeof(cur_model));

    char cur_homepage[128] = "https://lite.duckduckgo.com";
    nvs_handle_t nvs;
    if (nvs_open("web", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(cur_homepage);
        (void)nvs_get_str(nvs, "homepage", cur_homepage, &len);
        nvs_close(nvs);
    }
    char cur_home_esc[128];
    escape_html_str(cur_homepage, cur_home_esc, sizeof(cur_home_esc));

    solar_os_wifi_status_t wst;
    solar_os_wifi_get_status(&wst);
    char cur_ssid_esc[64] = "";
    escape_html_str(wst.connected ? wst.ssid : "", cur_ssid_esc, sizeof(cur_ssid_esc));

    const char *html_header =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>SolarOS Web Hub</title>"
        "<style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#edf2f7;margin:0;padding:24px;color:#2d3748}"
        ".card{max-width:920px;margin:0 auto;background:#fff;border-radius:12px;box-shadow:0 4px 16px rgba(0,0,0,0.08);padding:24px}"
        "h1{margin-top:0;display:flex;align-items:center;gap:12px;font-size:22px;color:#1a202c;border-bottom:2px solid #e2e8f0;padding-bottom:12px}"
        ".badge{background:#ebf8ff;color:#2b6cb0;padding:4px 10px;border-radius:6px;font-size:13px;font-weight:600}"
        ".tab-bar{display:flex;gap:10px;margin-bottom:20px;border-bottom:2px solid #e2e8f0;padding-bottom:12px}"
        ".tab-btn{background:#edf2f7;color:#4a5568;font-weight:600;padding:10px 20px;border-radius:8px;border:none;cursor:pointer;font-size:15px}"
        ".tab-btn.active{background:#3182ce;color:#fff}"
        ".upload-box{border:2px dashed #4299e1;border-radius:10px;padding:22px;text-align:center;background:#f7fafc;margin:20px 0;transition:background .2s}"
        ".upload-box.dragover{background:#ebf8ff;border-color:#2b6cb0}"
        ".prog-bar{width:100%;height:14px;background:#e2e8f0;border-radius:7px;overflow:hidden;margin-top:12px;display:none}"
        ".prog-fill{width:0%;height:100%;background:#38a169;transition:width .2s}"
        "select,input[type=file],input[type=text],input[type=password]{padding:8px 12px;border-radius:6px;border:1px solid #cbd5e0;font-size:14px;background:#fff;box-sizing:border-box}"
        ".form-group{margin-bottom:16px}"
        ".form-group label{display:block;margin-bottom:6px;font-weight:600;color:#4a5568;font-size:14px}"
        "button.btn-primary{background:#3182ce;color:#fff;font-weight:600;padding:9px 18px;border-radius:6px;border:none;cursor:pointer;font-size:14px}"
        "button.btn-primary:hover{background:#2b6cb0}"
        "button.btn-sec{background:#edf2f7;color:#2d3748;font-weight:600;padding:8px 14px;border-radius:6px;border:1px solid #cbd5e0;cursor:pointer;font-size:13px}"
        "button.btn-sec:hover{background:#e2e8f0}"
        ".folder-card{background:#fff;border:1px solid #e2e8f0;border-radius:8px;margin-bottom:16px;overflow:hidden;box-shadow:0 1px 3px rgba(0,0,0,0.04)}"
        ".folder-header{background:#edf2f7;padding:10px 14px;font-weight:700;color:#2d3748;border-bottom:1px solid #e2e8f0;display:flex;justify-content:space-between;align-items:center}"
        ".btn-sub{background:#fff;color:#2b6cb0;border:1px solid #cbd5e0;padding:4px 9px;border-radius:4px;font-size:12px;font-weight:600;cursor:pointer}"
        ".btn-sub-del{background:#fed7d7;color:#c53030;border:1px solid #feb2b2;padding:4px 9px;border-radius:4px;font-size:12px;font-weight:600;cursor:pointer}"
        ".file-table{width:100%;border-collapse:collapse}"
        ".file-table th,.file-table td{padding:9px 12px;text-align:left;border-bottom:1px solid #edf2f7;font-size:13px}"
        ".file-table th{background:#f7fafc;color:#718096;font-weight:600;font-size:12px;text-transform:uppercase}"
        ".act-btn{background:transparent;border:none;cursor:pointer;font-size:16px;padding:4px 6px;border-radius:6px;transition:background .2s;text-decoration:none;display:inline-block}"
        ".act-btn:hover{background:#edf2f7}"
        ".act-del:hover{background:#fed7d7}"
        ".act-cell{white-space:nowrap}"
        ".modal-overlay{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.5);z-index:999;align-items:center;justify-content:center}"
        ".modal-box{background:#fff;padding:24px;border-radius:12px;width:320px;max-width:90%;box-shadow:0 10px 25px rgba(0,0,0,0.15)}"
        ".batch-bar{background:#2b6cb0;color:#fff;padding:12px 18px;border-radius:8px;margin-bottom:18px;box-shadow:0 3px 8px rgba(0,0,0,0.15);display:none;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:12px}"
        ".settings-box{background:#f7fafc;border:1px solid #e2e8f0;border-radius:10px;padding:24px;margin-top:16px}"
        "</style></head><body><div class='card'>"
        "<h1>SolarOS Web Hub <span class='badge'>Device Control</span></h1>"
        "<div class='tab-bar'>"
        "<button class='tab-btn active' id='tabBtnFiles' onclick=\"switchTab('files')\">📁 Files & SD Card Storage</button>"
        "<button class='tab-btn' id='tabBtnSettings' onclick=\"switchTab('settings')\">⚙️ System & AI Settings</button>"
        "</div>"
        "<div id='tabFiles'>"
        "<div class='upload-box' id='dropZone'>"
        "<h3 style='margin-top:0'>Upload Multiple Files to SD Card</h3>"
        "<p style='font-size:13px;color:#718096;margin:4px 0 14px 0'>Select multiple files or drag & drop files here</p>"
        "<div style='margin-bottom:12px;display:flex;gap:10px;justify-content:center;align-items:center;flex-wrap:wrap;'>"
        "<label><strong>Target Directory:</strong></label> "
        "<select id='folderSelect' style='min-width:180px'>"
        "<option value=''>/ (SD Root)</option>";

    httpd_resp_send_chunk(req, html_header, HTTPD_RESP_USE_STRLEN);

    const char *mount = get_storage_mount();

    /* Render dynamic folder options for upload select */
    render_folder_option_tags(req, mount, "", sc);

    const char *upload_mid =
        "</select> "
        "<button class='btn-sec' onclick='createNewFolder()'>➕ New Folder</button>"
        "</div>"
        "<input type='file' id='fileInput' multiple style='margin-bottom:10px;' onchange='handleFileSelect(this)'><br>"
        "<button class='btn-primary' id='uploadBtn' onclick='uploadBatchFiles()'>Upload Files</button>"
        "<div class='prog-bar' id='progBar'><div class='prog-fill' id='progFill'></div></div>"
        "<div id='uploadStatus' style='margin-top:10px;font-weight:600;'></div>"
        "</div>"
        "<div id='batchToolbar' class='batch-bar'>"
        "<div style='display:flex;align-items:center;gap:12px;flex-wrap:wrap;'>"
        "<span><strong>Selected:</strong> <span id='selCount' class='badge' style='background:#fff;color:#2b6cb0;'>0 items</span></span>"
        "<button class='btn-sub-del' style='padding:6px 12px;font-size:13px;' onclick='batchDelete()'>🗑️ Delete Selected</button>"
        "<div style='display:flex;align-items:center;gap:6px;'>"
        "<label style='font-size:13px;font-weight:600;'>Move to:</label>"
        "<select id='batchMoveFolder' style='padding:4px 8px;font-size:13px;'>"
        "<option value=''>/ (SD Root)</option>";

    httpd_resp_send_chunk(req, upload_mid, HTTPD_RESP_USE_STRLEN);

    /* Render dynamic folder options for batch move select */
    render_folder_option_tags(req, mount, "", sc);

    const char *batch_mid2 =
        "</select>"
        "<button class='btn-sub' style='padding:6px 12px;font-size:13px;' onclick='batchMove()'>📦 Move Selected</button>"
        "</div>"
        "<button class='btn-sec' style='padding:6px 12px;font-size:13px;' onclick='clearSelection()'>✖ Deselect</button>"
        "</div></div>"
        "<h3>Hierarchical Directory Tree View</h3>";

    httpd_resp_send_chunk(req, batch_mid2, HTTPD_RESP_USE_STRLEN);

    /* 1. Root Directory */
    render_directory_tree_node(req, mount, "", sc);

    /* 2. Subdirectories */
    if (mount != NULL) {
        DIR *d = opendir(mount);
        if (d != NULL) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                char sub_path[256];
                snprintf(sub_path, sizeof(sub_path), "%s/%s", mount, ent->d_name);
                struct stat st;
                if (stat(sub_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                    render_directory_tree_node(req, mount, ent->d_name, sc);
                }
            }
            closedir(d);
        }
    }

    httpd_resp_send_chunk(req, "</div><div id='tabSettings' style='display:none'><div class='settings-box'>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "<h3 style='margin-top:0;color:#2b6cb0;'>🤖 AI Agent Configuration</h3>", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "<p style='font-size:13px;color:#718096;'>Paste your long API keys and custom server addresses directly from your PC/phone.</p>", HTTPD_RESP_USE_STRLEN);

    snprintf(sc->sbuf, sizeof(sc->sbuf),
             "<div class='form-group'><label>API Key (OpenAI, DeepSeek, Ollama):</label><input type='password' id='agentApiKey' placeholder='%s' style='width:100%%'></div>",
             agent_st.api_key_set ? "API Key is set (leave blank to keep)" : "Enter API Key (sk-...)");
    httpd_resp_send_chunk(req, sc->sbuf, HTTPD_RESP_USE_STRLEN);

    snprintf(sc->sbuf, sizeof(sc->sbuf),
             "<div class='form-group'><label>API Endpoint URL:</label><input type='text' id='agentEndpoint' value='%s' style='width:100%%'></div>",
             cur_endpoint);
    httpd_resp_send_chunk(req, sc->sbuf, HTTPD_RESP_USE_STRLEN);

    snprintf(sc->sbuf, sizeof(sc->sbuf),
             "<div class='form-group'><label>Model Name:</label><input type='text' id='agentModel' value='%s' style='width:100%%'></div>",
             cur_model);
    httpd_resp_send_chunk(req, sc->sbuf, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, "<hr style='border:none;border-top:1px solid #e2e8f0;margin:24px 0'><h3 style='color:#2b6cb0;'>🌐 Web Browser Configuration</h3>", HTTPD_RESP_USE_STRLEN);

    snprintf(sc->sbuf, sizeof(sc->sbuf),
             "<div class='form-group'><label>Default Homepage URL:</label><input type='text' id='webHomepage' value='%s' style='width:100%%'></div>",
             cur_home_esc);
    httpd_resp_send_chunk(req, sc->sbuf, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req, "<hr style='border:none;border-top:1px solid #e2e8f0;margin:24px 0'><h3 style='color:#2b6cb0;'>📶 Wi-Fi & Network Configuration</h3>", HTTPD_RESP_USE_STRLEN);

    snprintf(sc->sbuf, sizeof(sc->sbuf),
             "<div class='form-group'><label>Wi-Fi SSID:</label><input type='text' id='wifiSsid' value='%s' placeholder='Network name' style='width:100%%'></div>",
             cur_ssid_esc);
    httpd_resp_send_chunk(req, sc->sbuf, HTTPD_RESP_USE_STRLEN);

    httpd_resp_send_chunk(req,
        "<div class='form-group'><label>Wi-Fi Password:</label><input type='password' id='wifiPass' placeholder='Password (leave blank to keep current)' style='width:100%%'></div>"
        "<br><button class='btn-primary' onclick='saveSettings()'>💾 Save & Apply All Settings</button>"
        "<div id='settingsStatus' style='margin-top:14px;font-weight:600;'></div>"
        "</div></div>", HTTPD_RESP_USE_STRLEN);

    free(sc);

    const char *html_footer =
        "</div>"
        "<div id='moveModal' class='modal-overlay'>"
        "<div class='modal-box'>"
        "<h3 style='margin-top:0'>Move Item</h3>"
        "<p id='moveItemName' style='font-size:14px;word-break:break-all;color:#4a5568'></p>"
        "<label style='display:block;margin-bottom:8px;font-size:14px;font-weight:600;'>Destination Folder:</label>"
        "<select id='moveTargetSelect' style='width:100%;margin-bottom:20px;'></select>"
        "<input type='hidden' id='moveItemPath'>"
        "<div style='display:flex;justify-content:flex-end;gap:10px;'>"
        "<button class='btn-sec' onclick='closeMoveModal()'>Cancel</button>"
        "<button class='btn-primary' onclick='confirmMove()'>Move</button>"
        "</div></div></div>"
        "<script>"
        "function switchTab(t){"
        "  var isF=(t==='files');"
        "  document.getElementById('tabFiles').style.display=isF?'block':'none';"
        "  document.getElementById('tabSettings').style.display=isF?'none':'block';"
        "  document.getElementById('tabBtnFiles').className='tab-btn '+(isF?'active':'');"
        "  document.getElementById('tabBtnSettings').className='tab-btn '+(isF?'':'active');"
        "}"
        "function saveSettings(){"
        "  var st=document.getElementById('settingsStatus');"
        "  st.style.color='#3182ce';st.innerText='Saving settings to device...';"
        "  var params='api_key='+encodeURIComponent(document.getElementById('agentApiKey').value)+"
        "    '&endpoint='+encodeURIComponent(document.getElementById('agentEndpoint').value)+"
        "    '&model='+encodeURIComponent(document.getElementById('agentModel').value)+"
        "    '&homepage='+encodeURIComponent(document.getElementById('webHomepage').value)+"
        "    '&wifi_ssid='+encodeURIComponent(document.getElementById('wifiSsid').value)+"
        "    '&wifi_pass='+encodeURIComponent(document.getElementById('wifiPass').value);"
        "  var xhr=new XMLHttpRequest();"
        "  xhr.open('POST','/save_settings',true);"
        "  xhr.setRequestHeader('Content-Type','application/x-www-form-urlencoded');"
        "  xhr.onload=function(){if(xhr.status==200){st.style.color='#38a169';st.innerText='Settings saved & applied successfully!';}else{st.style.color='#e53e3e';st.innerText='Failed to save settings: '+xhr.status;}};"
        "  xhr.onerror=function(){st.style.color='#e53e3e';st.innerText='Network error while saving settings!';};"
        "  xhr.send(params);"
        "}"
        "function createNewFolder(){"
        "  var n=prompt('Enter new folder name:');"
        "  if(n && n.trim()){"
        "    location.href='/mkdir?folder='+encodeURIComponent(n.trim());"
        "  }"
        "}"
        "function newFolderIn(p){"
        "  var n=prompt('Enter new subfolder name inside /'+decodeURIComponent(p)+':');"
        "  if(n && n.trim()){"
        "    var full = p ? decodeURIComponent(p)+'/'+n.trim() : n.trim();"
        "    location.href='/mkdir?folder='+encodeURIComponent(full);"
        "  }"
        "}"
        "function renamePath(p, isDir){"
        "  var oldN=decodeURIComponent(p);"
        "  var curBase=oldN.substring(oldN.lastIndexOf('/')+1);"
        "  var newN=prompt('Rename '+(isDir?'folder':'file')+' to:', curBase);"
        "  if(newN && newN.trim() && newN.trim()!==curBase){"
        "    var parent=oldN.lastIndexOf('/')>=0 ? oldN.substring(0,oldN.lastIndexOf('/')) : '';"
        "    var fullNew = parent ? parent+'/'+newN.trim() : newN.trim();"
        "    location.href='/rename?old='+encodeURIComponent(oldN)+'&new='+encodeURIComponent(fullNew);"
        "  }"
        "}"
        "function movePath(p, isDir){"
        "  var oldN=decodeURIComponent(p);"
        "  document.getElementById('moveItemPath').value = oldN;"
        "  document.getElementById('moveItemName').innerText = oldN;"
        "  var sel = document.getElementById('moveTargetSelect');"
        "  sel.innerHTML = document.getElementById('folderSelect').innerHTML;"
        "  document.getElementById('moveModal').style.display = 'flex';"
        "}"
        "function closeMoveModal(){"
        "  document.getElementById('moveModal').style.display = 'none';"
        "}"
        "function confirmMove(){"
        "  var oldN = document.getElementById('moveItemPath').value;"
        "  var target = document.getElementById('moveTargetSelect').value;"
        "  location.href='/move?file='+encodeURIComponent(oldN)+'&to='+encodeURIComponent(target.trim());"
        "}"
        "function delFile(p, isDir){"
        "  if(confirm('Are you sure you want to delete '+(isDir?'folder':'file')+' '+decodeURIComponent(p)+'?')){"
        "    location.href='/delete?file='+p;"
        "  }"
        "}"
        "function getSelectedPaths(){"
        "  var chks=document.querySelectorAll('.file-chk:checked');"
        "  return Array.from(chks).map(function(c){return decodeURIComponent(c.getAttribute('data-path'));});"
        "}"
        "function updateSelection(){"
        "  var sel=getSelectedPaths();"
        "  var tb=document.getElementById('batchToolbar');"
        "  var cnt=document.getElementById('selCount');"
        "  if(sel.length>0){"
        "    tb.style.display='flex';"
        "    cnt.innerText=sel.length+' items';"
        "  } else {"
        "    tb.style.display='none';"
        "  }"
        "}"
        "function toggleFolderChecks(m){"
        "  var tbl=m.closest('table');"
        "  var chks=tbl.querySelectorAll('.file-chk');"
        "  chks.forEach(function(c){c.checked=m.checked;});"
        "  updateSelection();"
        "}"
        "function clearSelection(){"
        "  document.querySelectorAll('.file-chk, .folder-chk-all').forEach(function(c){c.checked=false;});"
        "  updateSelection();"
        "}"
        "function batchDelete(){"
        "  var sel=getSelectedPaths();"
        "  if(!sel.length) return;"
        "  if(!confirm('Are you sure you want to delete '+sel.length+' selected files?')) return;"
        "  var xhr=new XMLHttpRequest();"
        "  xhr.open('POST','/batch_delete',true);"
        "  xhr.setRequestHeader('Content-Type','application/x-www-form-urlencoded');"
        "  xhr.onload=function(){location.reload();};"
        "  xhr.onerror=function(){alert('Batch delete failed!');};"
        "  xhr.send('files='+encodeURIComponent(sel.join(',')));"
        "}"
        "function batchMove(){"
        "  var sel=getSelectedPaths();"
        "  if(!sel.length) return;"
        "  var dst=document.getElementById('batchMoveFolder').value;"
        "  var xhr=new XMLHttpRequest();"
        "  xhr.open('POST','/batch_move',true);"
        "  xhr.setRequestHeader('Content-Type','application/x-www-form-urlencoded');"
        "  xhr.onload=function(){location.reload();};"
        "  xhr.onerror=function(){alert('Batch move failed!');};"
        "  xhr.send('dst_folder='+encodeURIComponent(dst)+'&files='+encodeURIComponent(sel.join(',')));"
        "}"
        "function handleFileSelect(input){"
        "  var btn=document.getElementById('uploadBtn');"
        "  if(input.files && input.files.length>1){"
        "    btn.innerText='Upload '+input.files.length+' Files';"
        "  } else {"
        "    btn.innerText='Upload File';"
        "  }"
        "}"
        "var dz=document.getElementById('dropZone');"
        "dz.ondragover=function(e){e.preventDefault();dz.classList.add('dragover');};"
        "dz.ondragleave=function(){dz.classList.remove('dragover');};"
        "dz.ondrop=function(e){"
        "  e.preventDefault();dz.classList.remove('dragover');"
        "  if(e.dataTransfer.files && e.dataTransfer.files.length){"
        "    document.getElementById('fileInput').files=e.dataTransfer.files;"
        "    handleFileSelect(document.getElementById('fileInput'));"
        "  }"
        "};"
        "function uploadSingleFilePromise(file, folder, onProg){"
        "  return new Promise(function(resolve, reject){"
        "    var xhr=new XMLHttpRequest();"
        "    xhr.open('POST','/upload?folder='+encodeURIComponent(folder)+'&name='+encodeURIComponent(file.name),true);"
        "    xhr.upload.onprogress=function(e){if(e.lengthComputable){onProg(e.loaded/e.total);}};"
        "    xhr.onload=function(){if(xhr.status==200){resolve();}else{reject('HTTP '+xhr.status);}};"
        "    xhr.onerror=function(){reject('Network Error');};"
        "    xhr.send(file);"
        "  });"
        "}"
        "async function uploadBatchFiles(){"
        "  var fi=document.getElementById('fileInput');"
        "  if(!fi.files || !fi.files.length){alert('Please select files to upload!');return;}"
        "  var folder=document.getElementById('folderSelect').value;"
        "  var pb=document.getElementById('progBar');"
        "  var pf=document.getElementById('progFill');"
        "  var st=document.getElementById('uploadStatus');"
        "  pb.style.display='block';"
        "  pf.style.width='0%';"
        "  var total=fi.files.length;"
        "  var completed=0;"
        "  for(var i=0;i<total;i++){"
        "    var f=fi.files[i];"
        "    st.innerText='['+(i+1)+'/'+total+'] Uploading '+f.name+'...';"
        "    try{"
        "      await uploadSingleFilePromise(f, folder, function(filePct){"
        "        var overall = Math.round(((completed + filePct)/total)*100);"
        "        pf.style.width=overall+'%';"
        "      });"
        "      completed++;"
        "      pf.style.width=Math.round((completed/total)*100)+'%';"
        "    }catch(err){"
        "      st.style.color='#e53e3e';"
        "      st.innerText='Upload failed on '+f.name+': '+err;"
        "      return;"
        "    }"
        "  }"
        "  st.style.color='#38a169';"
        "  st.innerText='✅ All '+total+' files uploaded successfully!';"
        "  setTimeout(function(){location.reload();},1200);"
        "}"
        "</script></body></html>";

    httpd_resp_send_chunk(req, html_footer, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_handle_download(httpd_req_t *req, void *user)
{
    (void)user;
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char rel_path[160];
    if (httpd_query_key_value(query, "file", rel_path, sizeof(rel_path)) != ESP_OK) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    url_decode_inplace(rel_path);

    char full_path[192];
    const char *mount = get_storage_mount();
    snprintf(full_path, sizeof(full_path), "%s/%s", mount, rel_path);

    FILE *f = fopen(full_path, "rb");
    if (f == NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");

    const char *bname = strrchr(rel_path, '/');
    bname = (bname != NULL) ? bname + 1 : rel_path;

    char disp_hdr[160];
    snprintf(disp_hdr, sizeof(disp_hdr), "attachment; filename=\"%s\"", bname);
    httpd_resp_set_hdr(req, "Content-Disposition", disp_hdr);

    char chunk[512];
    size_t read_bytes;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        httpd_resp_send_chunk(req, chunk, read_bytes);
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);

    fserver.downloads_count++;
    strlcpy(fserver.last_file, bname, sizeof(fserver.last_file));
    return ESP_OK;
}

static void remove_directory_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (d == NULL) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char sub[192];
        snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(sub, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                remove_directory_recursive(sub);
            } else {
                unlink(sub);
            }
        }
    }
    closedir(d);
    rmdir(path);
}

static esp_err_t http_handle_delete(httpd_req_t *req, void *user)
{
    (void)user;
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char rel_path[160];
        if (httpd_query_key_value(query, "file", rel_path, sizeof(rel_path)) == ESP_OK) {
            url_decode_inplace(rel_path);
            char full_path[192];
            const char *mount = get_storage_mount();
            snprintf(full_path, sizeof(full_path), "%s/%s", mount, rel_path);

            struct stat st;
            if (stat(full_path, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    remove_directory_recursive(full_path);
                } else {
                    unlink(full_path);
                }
            }
        }
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_handle_batch_delete(httpd_req_t *req, void *user)
{
    (void)user;
    char buf[2048] = "";
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received > 0) {
        buf[received] = '\0';
        char files_str[2048] = "";
        if (httpd_query_key_value(buf, "files", files_str, sizeof(files_str)) == ESP_OK) {
            url_decode_inplace(files_str);
            const char *mount = get_storage_mount();
            char *token = strtok(files_str, ",");
            while (token != NULL) {
                while (*token == ' ') token++;
                if (token[0] != '\0') {
                    char full_path[256];
                    snprintf(full_path, sizeof(full_path), "%s/%s", mount, token);
                    struct stat st;
                    if (stat(full_path, &st) == 0) {
                        if (S_ISDIR(st.st_mode)) {
                            remove_directory_recursive(full_path);
                        } else {
                            unlink(full_path);
                        }
                    }
                }
                token = strtok(NULL, ",");
            }
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t http_handle_mkdir(httpd_req_t *req, void *user)
{
    (void)user;
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char folder[160];
        if (httpd_query_key_value(query, "folder", folder, sizeof(folder)) == ESP_OK) {
            url_decode_inplace(folder);
            if (folder[0] != '\0') {
                char full_path[192];
                const char *mount = get_storage_mount();
                snprintf(full_path, sizeof(full_path), "%s/%s", mount, folder);
                mkdir(full_path, 0777);
            }
        }
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_handle_rename(httpd_req_t *req, void *user)
{
    (void)user;
    char query[384];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char old_p[160] = "";
        char new_p[160] = "";
        if (httpd_query_key_value(query, "old", old_p, sizeof(old_p)) == ESP_OK &&
            httpd_query_key_value(query, "new", new_p, sizeof(new_p)) == ESP_OK) {
            url_decode_inplace(old_p);
            url_decode_inplace(new_p);
            if (old_p[0] != '\0' && new_p[0] != '\0') {
                char full_old[192];
                char full_new[192];
                const char *mount = get_storage_mount();
                snprintf(full_old, sizeof(full_old), "%s/%s", mount, old_p);
                snprintf(full_new, sizeof(full_new), "%s/%s", mount, new_p);
                rename(full_old, full_new);
            }
        }
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_handle_move(httpd_req_t *req, void *user)
{
    (void)user;
    char query[384];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char src_p[160] = "";
        char dst_folder[160] = "";
        (void)httpd_query_key_value(query, "file", src_p, sizeof(src_p));
        (void)httpd_query_key_value(query, "to", dst_folder, sizeof(dst_folder));

        url_decode_inplace(src_p);
        url_decode_inplace(dst_folder);

        if (src_p[0] != '\0') {
            const char *mount = get_storage_mount();
            char full_src[256];
            snprintf(full_src, sizeof(full_src), "%s/%s", mount, src_p);

            const char *bname = strrchr(src_p, '/');
            bname = (bname != NULL) ? bname + 1 : src_p;

            char full_dst[256];
            if (dst_folder[0] != '\0') {
                char target_dir[192];
                snprintf(target_dir, sizeof(target_dir), "%s/%s", mount, dst_folder);
                mkdir(target_dir, 0777);
                snprintf(full_dst, sizeof(full_dst), "%s/%s/%s", mount, dst_folder, bname);
            } else {
                snprintf(full_dst, sizeof(full_dst), "%s/%s", mount, bname);
            }

            rename(full_src, full_dst);
        }
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t http_handle_batch_move(httpd_req_t *req, void *user)
{
    (void)user;
    char buf[2048] = "";
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received > 0) {
        buf[received] = '\0';
        char dst_folder[160] = "";
        char files_str[2048] = "";
        (void)httpd_query_key_value(buf, "dst_folder", dst_folder, sizeof(dst_folder));
        (void)httpd_query_key_value(buf, "files", files_str, sizeof(files_str));

        url_decode_inplace(dst_folder);
        url_decode_inplace(files_str);

        const char *mount = get_storage_mount();
        if (dst_folder[0] != '\0') {
            char target_dir[192];
            snprintf(target_dir, sizeof(target_dir), "%s/%s", mount, dst_folder);
            mkdir(target_dir, 0777);
        }

        char *token = strtok(files_str, ",");
        while (token != NULL) {
            while (*token == ' ') token++;
            if (token[0] != '\0') {
                char full_src[256];
                snprintf(full_src, sizeof(full_src), "%s/%s", mount, token);

                const char *bname = strrchr(token, '/');
                bname = (bname != NULL) ? bname + 1 : token;

                char full_dst[256];
                if (dst_folder[0] != '\0') {
                    snprintf(full_dst, sizeof(full_dst), "%s/%s/%s", mount, dst_folder, bname);
                } else {
                    snprintf(full_dst, sizeof(full_dst), "%s/%s", mount, bname);
                }

                rename(full_src, full_dst);
            }
            token = strtok(NULL, ",");
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t http_handle_upload(httpd_req_t *req, void *user)
{
    (void)user;
    char query[256] = "";
    char folder[64] = "";
    char filename[96] = "upload.bin";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "folder", folder, sizeof(folder));
        (void)httpd_query_key_value(query, "name", filename, sizeof(filename));
    }

    url_decode_inplace(folder);
    url_decode_inplace(filename);

    const char *mount = get_storage_mount();
    char full_path[192];

    if (folder[0] != '\0') {
        char target_dir[128];
        snprintf(target_dir, sizeof(target_dir), "%s/%s", mount, folder);
        mkdir(target_dir, 0777);
        snprintf(full_path, sizeof(full_path), "%s/%s/%s", mount, folder, filename);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", mount, filename);
    }

    FILE *f = fopen(full_path, "wb");
    if (f == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char buf[512];
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) break;
        fwrite(buf, 1, received, f);
        remaining -= received;
    }
    fclose(f);

    fserver.uploads_count++;
    strlcpy(fserver.last_file, filename, sizeof(fserver.last_file));

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t http_handle_save_settings(httpd_req_t *req, void *user)
{
    (void)user;
    char buf[512] = "";
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received > 0) {
        buf[received] = '\0';

        char api_key[192] = "";
        char endpoint[256] = "";
        char model[96] = "";
        char homepage[160] = "";
        char wifi_ssid[36] = "";
        char wifi_pass[68] = "";

        if (httpd_query_key_value(buf, "api_key", api_key, sizeof(api_key)) == ESP_OK) {
            url_decode_inplace(api_key);
            if (api_key[0] != '\0') {
                (void)solar_os_agent_set_api_key(api_key);
            }
        }
        if (httpd_query_key_value(buf, "endpoint", endpoint, sizeof(endpoint)) == ESP_OK) {
            url_decode_inplace(endpoint);
            if (endpoint[0] != '\0') {
                (void)solar_os_agent_set_endpoint(endpoint);
            }
        }
        if (httpd_query_key_value(buf, "model", model, sizeof(model)) == ESP_OK) {
            url_decode_inplace(model);
            if (model[0] != '\0') {
                (void)solar_os_agent_set_model(model);
            }
        }
        if (httpd_query_key_value(buf, "homepage", homepage, sizeof(homepage)) == ESP_OK) {
            url_decode_inplace(homepage);
            if (homepage[0] != '\0') {
                nvs_handle_t nvs;
                if (nvs_open("web", NVS_READWRITE, &nvs) == ESP_OK) {
                    nvs_set_str(nvs, "homepage", homepage);
                    nvs_commit(nvs);
                    nvs_close(nvs);
                }
            }
        }
        if (httpd_query_key_value(buf, "wifi_ssid", wifi_ssid, sizeof(wifi_ssid)) == ESP_OK) {
            url_decode_inplace(wifi_ssid);
            if (httpd_query_key_value(buf, "wifi_pass", wifi_pass, sizeof(wifi_pass)) == ESP_OK) {
                url_decode_inplace(wifi_pass);
                if (wifi_ssid[0] != '\0') {
                    (void)solar_os_wifi_connect(wifi_ssid, wifi_pass);
                }
            }
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Settings saved successfully!\"}");
    return ESP_OK;
}

static const solar_os_http_route_t routes[] = {
    {"file_server", "/", HTTP_GET, false, SOLAR_OS_HTTP_AUTH_PUBLIC, http_handle_root, NULL},
    {"file_server", "/download", HTTP_GET, false, SOLAR_OS_HTTP_AUTH_PUBLIC, http_handle_download, NULL},
    {"file_server", "/delete", HTTP_GET, false, SOLAR_OS_HTTP_AUTH_PUBLIC, http_handle_delete, NULL},
    {"file_server", "/batch_delete", HTTP_POST, false, SOLAR_OS_HTTP_AUTH_PUBLIC, http_handle_batch_delete, NULL},
    {"file_server", "/mkdir", HTTP_GET, false, SOLAR_OS_HTTP_AUTH_PUBLIC, http_handle_mkdir, NULL},
    {"file_server", "/rename", HTTP_GET, false, SOLAR_OS_HTTP_AUTH_PUBLIC, http_handle_rename, NULL},
    {"file_server", "/move", HTTP_GET, false, SOLAR_OS_HTTP_AUTH_PUBLIC, http_handle_move, NULL},
    {"file_server", "/batch_move", HTTP_POST, false, SOLAR_OS_HTTP_AUTH_PUBLIC, http_handle_batch_move, NULL},
    {"file_server", "/upload", HTTP_POST, false, SOLAR_OS_HTTP_AUTH_PUBLIC, http_handle_upload, NULL},
    {"file_server", "/save_settings", HTTP_POST, false, SOLAR_OS_HTTP_AUTH_PUBLIC, http_handle_save_settings, NULL},
};
#define ROUTE_COUNT (sizeof(routes)/sizeof(routes[0]))

static void file_server_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    const int screen_w = (int)solar_os_gfx_width(gfx);
    const int screen_h = (int)solar_os_gfx_height(gfx);

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    /* 1. Shared header. */
    solar_os_appbar_header_t header = {0};
    header.title = "File Server";
    header.show_back = true;
    header.status_line = "HTTP port 80  -  runs in background";
    solar_os_appbar_draw_header(gfx, &header);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);

    /* 2. Main Hero Box */
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 16, 36, screen_w - 32, 115);
    solar_os_gfx_rect(gfx, 18, 38, screen_w - 36, 111);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 30, 60, "OPEN IN YOUR PHONE OR PC BROWSER:");

    char url_str[64];
    snprintf(url_str, sizeof(url_str), "http://%s/", fserver.ip_str[0] ? fserver.ip_str : "192.168.1.xxx");
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_20);
    solar_os_gfx_text(gfx, 40, 95, url_str);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 30, 130, "* Batch Upload & Delete, Folder Move, ROMs, Videos, Music");

    /* 3. Stats Box */
    solar_os_gfx_rect(gfx, 16, 162, screen_w - 32, 100);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 28, 182, "LIVE ACTIVITY & STORAGE STATUS");
    solar_os_gfx_line(gfx, 24, 188, screen_w - 24, 188);

    char stats_line1[64];
    snprintf(stats_line1, sizeof(stats_line1), "Uploaded Files: %u    Downloaded Files: %u",
             (unsigned)fserver.uploads_count, (unsigned)fserver.downloads_count);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 28, 208, stats_line1);

    char stats_line2[64];
    snprintf(stats_line2, sizeof(stats_line2), "Last Action: %s",
             fserver.last_file[0] ? fserver.last_file : "Server listening on HTTP port 80");
    solar_os_gfx_text(gfx, 28, 230, stats_line2);

    char sd_stat[64];
    snprintf(sd_stat, sizeof(sd_stat), "SD Card: %s (Mounted at /sdcard)",
             solar_os_storage_sd_is_mounted() ? "READY" : "NOT DETECTED");
    solar_os_gfx_text(gfx, 28, 250, sd_stat);

    /* 4. Shared footer chips. */
    solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    const size_t count = fserver_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
    const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
    solar_os_appbar_draw_footer(gfx, &shortcuts);

    if (fserver.show_help) {
        solar_os_help_draw(gfx, "File Server - Help", fserver_help_lines, FSERVER_HELP_LINE_COUNT);
    }

    solar_os_gfx_present(gfx);
}

static esp_err_t file_server_start(solar_os_context_t *ctx)
{
    memset(&fserver, 0, sizeof(fserver));
    solar_os_context_set_graphics_active(ctx, true);

    solar_os_wifi_status_t wst;
    solar_os_wifi_get_status(&wst);
    printf("[FILE-SERVER] file_server_start: wifi connected=%d, ip=%s\n",
           (int)wst.connected, wst.connected ? wst.ip : "(none)");
    fflush(stdout);

    SOLAR_OS_LOGI("file_server", "wifi connected=%d  ip=%s",
                  (int)wst.connected, wst.connected ? wst.ip : "(none)");
    if (wst.connected) {
        strlcpy(fserver.ip_str, wst.ip, sizeof(fserver.ip_str));
    } else {
        strlcpy(fserver.ip_str, "Connect Wi-Fi First", sizeof(fserver.ip_str));
    }
    fserver.port = 80;

    /* Register all HTTP routes */
    printf("[FILE-SERVER] Registering %u HTTP routes...\n", (unsigned)ROUTE_COUNT);
    fflush(stdout);
    SOLAR_OS_LOGI("file_server", "registering %u routes...", (unsigned)ROUTE_COUNT);
    esp_err_t route_err = ESP_OK;
    for (size_t i = 0; i < ROUTE_COUNT; i++) {
        route_err = solar_os_http_server_register_route(&routes[i]);
        if (route_err != ESP_OK) {
            printf("[FILE-SERVER] ERROR: route[%u] (%s %s) failed: %s\n",
                   (unsigned)i, routes[i].method == HTTP_GET ? "GET" : "POST",
                   routes[i].uri, esp_err_to_name(route_err));
            fflush(stdout);
            SOLAR_OS_LOGE("file_server", "route[%u] register failed: %s",
                          (unsigned)i, esp_err_to_name(route_err));
            (void)solar_os_http_server_unregister_owner("file_server");
            break;
        }
    }
    fserver.running = (route_err == ESP_OK);
    if (fserver.running) {
        printf("[FILE-SERVER] SUCCESS: File server running at http://%s/\n", fserver.ip_str);
        fflush(stdout);
        SOLAR_OS_LOGI("file_server", "server started OK — port=80 ip=%s", fserver.ip_str);
    } else {
        printf("[FILE-SERVER] FAILED to start file server: %s\n", esp_err_to_name(route_err));
        fflush(stdout);
        SOLAR_OS_LOGE("file_server", "server FAILED to start: %s", esp_err_to_name(route_err));
        strlcpy(fserver.ip_str, "Server failed to start!", sizeof(fserver.ip_str));
    }

    file_server_render(ctx);
    return ESP_OK;
}

static void file_server_stop(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, false);
}

static bool file_server_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (now - fserver.last_tick_ms > 2000) {
            fserver.last_tick_ms = now;
            solar_os_wifi_status_t wst;
            solar_os_wifi_get_status(&wst);
            if (wst.connected) {
                if (strcmp(fserver.ip_str, wst.ip) != 0) {
                    strlcpy(fserver.ip_str, wst.ip, sizeof(fserver.ip_str));
                    file_server_render(ctx);
                }
            }
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return true;
        const int16_t px = event->data.click.x;
        const int16_t py = event->data.click.y;

        if (fserver.show_help) {
            fserver.show_help = false;
            file_server_render(ctx);
            return true;
        }

        solar_os_appbar_header_t header = {0};
        header.show_back = true;
        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, px, py, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                solar_os_context_request_exit(ctx);
            }
            return true;
        }

        solar_os_appbar_shortcut_t items[SOLAR_OS_APPBAR_SHORTCUT_MAX];
        const size_t count = fserver_build_footer(items, SOLAR_OS_APPBAR_SHORTCUT_MAX);
        const solar_os_appbar_shortcuts_t shortcuts = { .items = items, .count = count };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &shortcuts, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM && fhit.index < count) {
                if (items[fhit.index].key == 'r') {
                    solar_os_wifi_status_t wst;
                    solar_os_wifi_get_status(&wst);
                    if (wst.connected) strlcpy(fserver.ip_str, wst.ip, sizeof(fserver.ip_str));
                } else {
                    fserver.show_help = true;
                }
                file_server_render(ctx);
            }
            return true;
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        const char ch = event->data.ch;

        if (fserver.show_help) {
            fserver.show_help = false;
            file_server_render(ctx);
            return true;
        }
        if (solar_os_help_char_opens(ch)) {
            fserver.show_help = true;
            file_server_render(ctx);
            return true;
        }
        if (ch == 'r' || ch == 'R') {
            file_server_render(ctx);
            return true;
        }
        if (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') {
            solar_os_context_request_exit(ctx);
            return true;
        }
    }

    return false;
}

const solar_os_app_t solar_os_file_server_app = {
    .name = "web_files",
    .summary = "Wi-Fi SD card HTTP file server",
    .flags = 0,
    .start = file_server_start,
    .stop = file_server_stop,
    .event = file_server_event,
    .state_slot = &file_server_state_ptr,
    .state_size = sizeof(file_server_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 500U,
    .worker_stack_bytes = 0,
};
