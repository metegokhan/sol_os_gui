#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_link.h"
#include "solar_os_port.h"

#define SOLAR_OS_LINK_STREAM_MAX 2U

typedef struct {
    char port[SOLAR_OS_PORT_NAME_MAX];
    char link[SOLAR_OS_LINK_NAME_MAX];
    uint8_t protocol_version;
    uint32_t peer_id;
    bool port_open;
    bool connected;
    size_t data_mtu;
    size_t rx_queued;
    size_t tx_queued;
    size_t tx_inflight;
    uint32_t bytes_received;
    uint32_t bytes_sent;
    uint32_t frames_received;
    uint32_t frames_sent;
    uint32_t acknowledgements_received;
    uint32_t acknowledgements_sent;
    uint32_t retries;
    uint32_t reconnects;
    uint32_t dropped;
    uint32_t decode_errors;
    esp_err_t last_error;
} solar_os_link_stream_status_t;

esp_err_t solar_os_link_stream_init(void);
esp_err_t solar_os_link_stream_create(const char *link,
                                      const char *port,
                                      uint32_t peer_id);
esp_err_t solar_os_link_stream_remove(const char *port);
size_t solar_os_link_stream_count(void);
bool solar_os_link_stream_get(size_t index,
                              solar_os_link_stream_status_t *status);
esp_err_t solar_os_link_stream_get_status(const char *port,
                                          solar_os_link_stream_status_t *status);

esp_err_t solar_os_link_stream_ingest(const char *link,
                                      const solar_os_link_message_t *message,
                                      uint32_t now_ms);
void solar_os_link_stream_process(const char *link, uint32_t now_ms);
void solar_os_link_stream_transport_stopped(const char *link);
