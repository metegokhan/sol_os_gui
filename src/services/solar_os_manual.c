#include "solar_os_manual.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

#include "solar_os_config.h"

#define MANUAL_SEARCH_MAX 12U
#define MANUAL_TOKEN_MAX 31U

#include "solar_os_manual_data.h"

size_t solar_os_manual_count(void)
{
    return SOLAR_OS_MANUAL_GENERATED_PAGE_COUNT;
}

const solar_os_manual_page_t *solar_os_manual_get(size_t index)
{
    return index < solar_os_manual_count() ?
        &SOLAR_OS_MANUAL_GENERATED_PAGES[index] : NULL;
}

static bool manual_alias_matches(const char *aliases, const char *name)
{
    if (aliases == NULL || name == NULL || name[0] == '\0') {
        return false;
    }
    const size_t name_len = strlen(name);
    const char *alias = aliases;
    while (*alias != '\0') {
        const char *end = strchr(alias, '\n');
        const size_t alias_len = end != NULL ? (size_t)(end - alias) : strlen(alias);
        if (alias_len == name_len && strncasecmp(alias, name, name_len) == 0) {
            return true;
        }
        if (end == NULL) {
            break;
        }
        alias = end + 1;
    }
    return false;
}

const solar_os_manual_page_t *solar_os_manual_find(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < solar_os_manual_count(); i++) {
        const solar_os_manual_page_t *page = solar_os_manual_get(i);
        if (page != NULL && strcasecmp(page->id, name) == 0) {
            return page;
        }
    }

    const solar_os_manual_page_t *match = NULL;
    for (size_t i = 0; i < solar_os_manual_count(); i++) {
        const solar_os_manual_page_t *page = solar_os_manual_get(i);
        if (page == NULL || !manual_alias_matches(page->aliases, name)) {
            continue;
        }
        if (match != NULL) {
            return NULL;
        }
        match = page;
    }
    return match;
}

static bool manual_contains_ci(const char *text, const char *needle)
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

static bool manual_stop_word(const char *token)
{
    static const char *const words[] = {
        "a", "an", "and", "for", "how", "in", "of", "on", "the", "to", "use", "with",
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strcmp(token, words[i]) == 0) {
            return true;
        }
    }
    return false;
}

static unsigned manual_score(const solar_os_manual_page_t *page, const char *query)
{
    if (page == NULL || query == NULL || query[0] == '\0') {
        return 0U;
    }
    if (strcasecmp(page->id, query) == 0) {
        return 100000U;
    }
    if (manual_alias_matches(page->aliases, query)) {
        return 50000U;
    }

    unsigned score = 0U;
    char token[MANUAL_TOKEN_MAX + 1U];
    size_t token_len = 0U;
    for (const unsigned char *cursor = (const unsigned char *)query;; cursor++) {
        const bool separator = *cursor == '\0' || !isalnum(*cursor);
        if (!separator && token_len < MANUAL_TOKEN_MAX) {
            token[token_len++] = (char)tolower(*cursor);
        }
        if (separator && token_len > 0U) {
            token[token_len] = '\0';
            if (!manual_stop_word(token)) {
                if (manual_contains_ci(page->id, token)) {
                    score += 900U;
                }
                if (manual_contains_ci(page->aliases, token)) {
                    score += 700U;
                }
                if (manual_contains_ci(page->title, token)) {
                    score += 500U;
                }
                if (manual_contains_ci(page->keywords, token)) {
                    score += 300U;
                }
                if (manual_contains_ci(page->summary, token)) {
                    score += 100U;
                }
                if (manual_contains_ci(page->contract, token)) {
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

size_t solar_os_manual_search(const char *query,
                              const solar_os_manual_page_t **results,
                              size_t capacity)
{
    if (query == NULL || query[0] == '\0' || results == NULL || capacity == 0U) {
        return 0U;
    }
    if (capacity > MANUAL_SEARCH_MAX) {
        capacity = MANUAL_SEARCH_MAX;
    }

    unsigned scores[MANUAL_SEARCH_MAX] = {0};
    size_t count = 0U;
    for (size_t candidate = 0U; candidate < solar_os_manual_count(); candidate++) {
        const solar_os_manual_page_t *page = solar_os_manual_get(candidate);
        const unsigned score = manual_score(page, query);
        if (score == 0U) {
            continue;
        }
        size_t insert = 0U;
        while (insert < count &&
               (scores[insert] > score ||
                (scores[insert] == score &&
                 strcmp(results[insert]->id, page->id) < 0))) {
            insert++;
        }
        if (insert >= capacity) {
            continue;
        }
        if (count < capacity) {
            count++;
        }
        for (size_t move = count - 1U; move > insert; move--) {
            results[move] = results[move - 1U];
            scores[move] = scores[move - 1U];
        }
        results[insert] = page;
        scores[insert] = score;
    }
    return count;
}
