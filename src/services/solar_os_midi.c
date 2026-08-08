#include "solar_os_midi.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SOLAR_OS_MIDI_SUBSCRIBER_MAX 4U
#define SOLAR_OS_MIDI_RX_QUEUE_DEPTH 32U
#define SOLAR_OS_MIDI_TX_QUEUE_DEPTH 32U

typedef struct {
    bool active;
    uint32_t token;
    char owner[SOLAR_OS_MIDI_OWNER_MAX];
    solar_os_midi_message_t queue[SOLAR_OS_MIDI_RX_QUEUE_DEPTH];
    size_t head;
    size_t count;
} solar_os_midi_subscriber_t;

static solar_os_midi_subscriber_t midi_subscribers[SOLAR_OS_MIDI_SUBSCRIBER_MAX];
static solar_os_midi_message_t midi_tx_queue[SOLAR_OS_MIDI_TX_QUEUE_DEPTH];
static size_t midi_tx_head;
static size_t midi_tx_count;
static solar_os_midi_status_t midi_status;
static SemaphoreHandle_t midi_mutex;
static StaticSemaphore_t midi_mutex_buffer;
static uint32_t midi_next_token;

static esp_err_t midi_ensure_mutex(void)
{
    if (midi_mutex == NULL) {
        midi_mutex = xSemaphoreCreateMutexStatic(&midi_mutex_buffer);
    }
    return midi_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static uint32_t midi_allocate_token(void)
{
    midi_next_token++;
    if (midi_next_token == 0U) {
        midi_next_token++;
    }
    return midi_next_token;
}

esp_err_t solar_os_midi_subscribe(const char *owner,
                                  solar_os_midi_subscription_t *subscription)
{
    if (owner == NULL || owner[0] == '\0' || subscription == NULL ||
        strnlen(owner, SOLAR_OS_MIDI_OWNER_MAX) >= SOLAR_OS_MIDI_OWNER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = midi_ensure_mutex();
    if (error != ESP_OK) {
        return error;
    }
    *subscription = (solar_os_midi_subscription_t)SOLAR_OS_MIDI_SUBSCRIPTION_INIT;
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_MIDI_SUBSCRIBER_MAX; i++) {
        if (midi_subscribers[i].active) {
            continue;
        }
        memset(&midi_subscribers[i], 0, sizeof(midi_subscribers[i]));
        midi_subscribers[i].active = true;
        midi_subscribers[i].token = midi_allocate_token();
        strlcpy(midi_subscribers[i].owner, owner, sizeof(midi_subscribers[i].owner));
        subscription->index = (int)i;
        subscription->token = midi_subscribers[i].token;
        xSemaphoreGive(midi_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(midi_mutex);
    return ESP_ERR_NO_MEM;
}

esp_err_t solar_os_midi_unsubscribe(solar_os_midi_subscription_t *subscription)
{
    if (subscription == NULL || subscription->index < 0 ||
        subscription->index >= (int)SOLAR_OS_MIDI_SUBSCRIBER_MAX ||
        subscription->token == 0U || midi_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    solar_os_midi_subscriber_t *subscriber =
        &midi_subscribers[(size_t)subscription->index];
    if (!subscriber->active || subscriber->token != subscription->token) {
        xSemaphoreGive(midi_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    memset(subscriber, 0, sizeof(*subscriber));
    *subscription = (solar_os_midi_subscription_t)SOLAR_OS_MIDI_SUBSCRIPTION_INIT;
    xSemaphoreGive(midi_mutex);
    return ESP_OK;
}

esp_err_t solar_os_midi_receive(solar_os_midi_subscription_t *subscription,
                                solar_os_midi_message_t *message)
{
    if (subscription == NULL || message == NULL || subscription->index < 0 ||
        subscription->index >= (int)SOLAR_OS_MIDI_SUBSCRIBER_MAX ||
        subscription->token == 0U || midi_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    solar_os_midi_subscriber_t *subscriber =
        &midi_subscribers[(size_t)subscription->index];
    if (!subscriber->active || subscriber->token != subscription->token) {
        xSemaphoreGive(midi_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (subscriber->count == 0U) {
        xSemaphoreGive(midi_mutex);
        return ESP_ERR_TIMEOUT;
    }
    *message = subscriber->queue[subscriber->head];
    subscriber->head = (subscriber->head + 1U) % SOLAR_OS_MIDI_RX_QUEUE_DEPTH;
    subscriber->count--;
    xSemaphoreGive(midi_mutex);
    return ESP_OK;
}

esp_err_t solar_os_midi_send(const solar_os_midi_message_t *message)
{
    if (!solar_os_midi_message_valid(message)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = midi_ensure_mutex();
    if (error != ESP_OK) {
        return error;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    if (!midi_status.running) {
        xSemaphoreGive(midi_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (midi_tx_count >= SOLAR_OS_MIDI_TX_QUEUE_DEPTH) {
        midi_status.tx_drops++;
        xSemaphoreGive(midi_mutex);
        return ESP_ERR_NO_MEM;
    }
    const size_t tail = (midi_tx_head + midi_tx_count) % SOLAR_OS_MIDI_TX_QUEUE_DEPTH;
    midi_tx_queue[tail] = *message;
    midi_tx_count++;
    xSemaphoreGive(midi_mutex);
    return ESP_OK;
}

void solar_os_midi_get_status(solar_os_midi_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (midi_ensure_mutex() != ESP_OK) {
        status->last_error = ESP_ERR_NO_MEM;
        return;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    *status = midi_status;
    xSemaphoreGive(midi_mutex);
}

esp_err_t solar_os_midi_worker_start(const char *bus_name)
{
    if (bus_name == NULL || bus_name[0] == '\0' ||
        strnlen(bus_name, SOLAR_OS_BUS_NAME_MAX) >= SOLAR_OS_BUS_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = midi_ensure_mutex();
    if (error != ESP_OK) {
        return error;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    memset(&midi_status, 0, sizeof(midi_status));
    midi_status.running = true;
    strlcpy(midi_status.bus_name, bus_name, sizeof(midi_status.bus_name));
    midi_tx_head = 0U;
    midi_tx_count = 0U;
    xSemaphoreGive(midi_mutex);
    return ESP_OK;
}

void solar_os_midi_worker_stop(void)
{
    if (midi_ensure_mutex() != ESP_OK) {
        return;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    midi_status.running = false;
    midi_tx_head = 0U;
    midi_tx_count = 0U;
    for (size_t i = 0; i < SOLAR_OS_MIDI_SUBSCRIBER_MAX; i++) {
        midi_subscribers[i].head = 0U;
        midi_subscribers[i].count = 0U;
    }
    xSemaphoreGive(midi_mutex);
}

void solar_os_midi_worker_publish(const solar_os_midi_message_t *message)
{
    if (!solar_os_midi_message_valid(message) || midi_ensure_mutex() != ESP_OK) {
        return;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    midi_status.rx_messages++;
    for (size_t i = 0; i < SOLAR_OS_MIDI_SUBSCRIBER_MAX; i++) {
        solar_os_midi_subscriber_t *subscriber = &midi_subscribers[i];
        if (!subscriber->active) {
            continue;
        }
        if (subscriber->count >= SOLAR_OS_MIDI_RX_QUEUE_DEPTH) {
            subscriber->head = (subscriber->head + 1U) % SOLAR_OS_MIDI_RX_QUEUE_DEPTH;
            subscriber->count--;
            midi_status.subscriber_drops++;
        }
        const size_t tail =
            (subscriber->head + subscriber->count) % SOLAR_OS_MIDI_RX_QUEUE_DEPTH;
        subscriber->queue[tail] = *message;
        subscriber->count++;
    }
    xSemaphoreGive(midi_mutex);
}

bool solar_os_midi_worker_take_tx(solar_os_midi_message_t *message)
{
    if (message == NULL || midi_ensure_mutex() != ESP_OK) {
        return false;
    }
    xSemaphoreTake(midi_mutex, portMAX_DELAY);
    if (!midi_status.running || midi_tx_count == 0U) {
        xSemaphoreGive(midi_mutex);
        return false;
    }
    *message = midi_tx_queue[midi_tx_head];
    midi_tx_head = (midi_tx_head + 1U) % SOLAR_OS_MIDI_TX_QUEUE_DEPTH;
    midi_tx_count--;
    xSemaphoreGive(midi_mutex);
    return true;
}

void solar_os_midi_worker_note_rx_bytes(size_t count)
{
    if (midi_ensure_mutex() == ESP_OK) {
        xSemaphoreTake(midi_mutex, portMAX_DELAY);
        midi_status.rx_bytes += (uint32_t)count;
        xSemaphoreGive(midi_mutex);
    }
}

void solar_os_midi_worker_note_tx(size_t count)
{
    if (midi_ensure_mutex() == ESP_OK) {
        xSemaphoreTake(midi_mutex, portMAX_DELAY);
        midi_status.tx_bytes += (uint32_t)count;
        midi_status.tx_messages++;
        midi_status.last_error = ESP_OK;
        xSemaphoreGive(midi_mutex);
    }
}

void solar_os_midi_worker_note_unsupported(void)
{
    if (midi_ensure_mutex() == ESP_OK) {
        xSemaphoreTake(midi_mutex, portMAX_DELAY);
        midi_status.parser_unsupported++;
        xSemaphoreGive(midi_mutex);
    }
}

void solar_os_midi_worker_note_error(esp_err_t error)
{
    if (midi_ensure_mutex() == ESP_OK) {
        xSemaphoreTake(midi_mutex, portMAX_DELAY);
        midi_status.last_error = error;
        xSemaphoreGive(midi_mutex);
    }
}
