#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct {
    const char *id;
    const char *title;
    const char *section;
    const char *summary;
    const char *aliases;
    const char *keywords;
    const char *body;
    const char *contract;
} solar_os_manual_page_t;

esp_err_t solar_os_manual_load_body(const solar_os_manual_page_t *page,
                                    const char **body,
                                    size_t *body_len,
                                    bool *owned);
esp_err_t solar_os_manual_load_contract(const solar_os_manual_page_t *page,
                                        const char **contract,
                                        size_t *contract_len,
                                        bool *owned);
void solar_os_manual_release_text(const char *text, bool owned);

size_t solar_os_manual_count(void);
const solar_os_manual_page_t *solar_os_manual_get(size_t index);
const solar_os_manual_page_t *solar_os_manual_find(const char *name);
size_t solar_os_manual_search(const char *query,
                              const solar_os_manual_page_t **results,
                              size_t capacity);
