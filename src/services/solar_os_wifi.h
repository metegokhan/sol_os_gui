#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_WIFI_SSID_MAX 32
#define SOLAR_OS_WIFI_PASSWORD_MAX 64
#define SOLAR_OS_WIFI_SCAN_MAX_RESULTS 12
#define SOLAR_OS_WIFI_AUTH_MAX 18
#define SOLAR_OS_WIFI_PROFILE_MAX 5

typedef enum {
    SOLAR_OS_WIFI_STATE_OFF,
    SOLAR_OS_WIFI_STATE_IDLE,
    SOLAR_OS_WIFI_STATE_SCANNING,
    SOLAR_OS_WIFI_STATE_CONNECTING,
    SOLAR_OS_WIFI_STATE_CONNECTED,
    SOLAR_OS_WIFI_STATE_DISCONNECTED,
    SOLAR_OS_WIFI_STATE_FAILED,
} solar_os_wifi_state_t;

typedef struct {
    char ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
    char auth[18];
    int8_t rssi;
    uint8_t channel;
    bool hidden;
} solar_os_wifi_ap_t;

typedef struct {
    solar_os_wifi_state_t state;
    bool initialized;
    bool started;
    bool connected;
    bool has_ip;
    bool has_saved_config;
    bool has_saved_ap_config;
    bool nat_enabled;
    bool nat_active;
    bool ap_enabled;
    bool ap_running;
    char ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
    char saved_ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
    char saved_ap_ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
    char saved_ap_auth[SOLAR_OS_WIFI_AUTH_MAX];
    char ip[16];
    char gateway[16];
    char netmask[16];
    char ap_ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
    char ap_auth[SOLAR_OS_WIFI_AUTH_MAX];
    char ap_ip[16];
    int8_t rssi;
    uint8_t channel;
    uint8_t disconnect_reason;
    uint8_t ap_channel;
    uint8_t ap_station_count;
    uint8_t ap_max_connections;
    uint8_t saved_profile_count;
    esp_err_t nat_last_error;
} solar_os_wifi_status_t;

typedef struct {
    char ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
    bool preferred;
} solar_os_wifi_profile_t;

esp_err_t solar_os_wifi_init(void);
esp_err_t solar_os_wifi_start(void);
esp_err_t solar_os_wifi_stop(void);
esp_err_t solar_os_wifi_prepare_sleep(void);
esp_err_t solar_os_wifi_resume(void);
esp_err_t solar_os_wifi_connect(const char *ssid, const char *password);
esp_err_t solar_os_wifi_connect_saved(void);
esp_err_t solar_os_wifi_disconnect(void);
esp_err_t solar_os_wifi_forget(void);
esp_err_t solar_os_wifi_forget_ssid(const char *ssid);
esp_err_t solar_os_wifi_forget_all(void);
esp_err_t solar_os_wifi_known(solar_os_wifi_profile_t *profiles, size_t max_profiles, size_t *count);
bool solar_os_wifi_is_known_ssid(const char *ssid);
esp_err_t solar_os_wifi_ap_start(const char *ssid, const char *password, const char *auth);
esp_err_t solar_os_wifi_ap_stop(void);
esp_err_t solar_os_wifi_nat_set(bool enabled);
esp_err_t solar_os_wifi_scan(solar_os_wifi_ap_t *aps, size_t max_aps, size_t *found);
void solar_os_wifi_get_status(solar_os_wifi_status_t *status);
void solar_os_wifi_get_status_text(char *buffer, size_t len);
const char *solar_os_wifi_state_name(solar_os_wifi_state_t state);
