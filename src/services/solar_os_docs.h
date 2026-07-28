#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define SOLAR_OS_DOCS_REVISION_MAX 17U
#define SOLAR_OS_DOCS_ERROR_MAX 96U

typedef struct {
    bool available;
    bool updating;
    char version[32];
    char revision[SOLAR_OS_DOCS_REVISION_MAX];
    size_t page_count;
    char last_error[SOLAR_OS_DOCS_ERROR_MAX];
} solar_os_docs_status_t;

esp_err_t solar_os_docs_init(void);
esp_err_t solar_os_docs_get_status(solar_os_docs_status_t *status);
esp_err_t solar_os_docs_update(void);
esp_err_t solar_os_docs_reset(void);

/*
 * Copies the path of an active, verified Markdown page. The returned path is
 * stable even if the documentation package is reset while a reader is open:
 * activated revision directories are immutable and are not deleted at runtime.
 */
esp_err_t solar_os_docs_page_path(const char *id, char *path, size_t path_len);
