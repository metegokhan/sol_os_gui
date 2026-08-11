#include "solar_os_flash_app.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_bus_types.h"
#include "solar_os_flash.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_queue.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"
#include "solar_os_terminal.h"
#include "solar_os_uart.h"

#define FLASH_APP_TASK_STACK 16384U
#define FLASH_APP_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define FLASH_APP_EVENT_QUEUE_LEN 12U

static const char *TAG = "solar_os_flash";

SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(FLASH_APP_TASK_STACK);

typedef enum {
  FLASH_APP_OPERATION_NONE,
  FLASH_APP_OPERATION_REFRESH,
  FLASH_APP_OPERATION_DOWNLOAD,
  FLASH_APP_OPERATION_PROGRAM,
} flash_app_operation_t;

typedef enum {
  FLASH_APP_EVENT_PROGRESS,
  FLASH_APP_EVENT_DONE,
} flash_app_event_type_t;

typedef struct {
  flash_app_event_type_t type;
  solar_os_flash_progress_t progress;
  esp_err_t result;
} flash_app_event_t;

typedef struct {
  solar_os_context_t *ctx;
  solar_os_shell_io_t fallback_io;
  solar_os_flash_catalog_t *catalog;
  solar_os_flash_artifact_t artifact;
  solar_os_flash_program_options_t program;
  char port[SOLAR_OS_BUS_NAME_MAX];
  char message[128];
  size_t cursor;
  flash_app_operation_t operation;
  QueueHandle_t events;
  TaskHandle_t task;
  volatile bool task_done;
  bool running;
  bool command_mode;
  bool command_exit_requested;
  bool result_screen;
  bool result_received;
  volatile solar_os_flash_progress_stage_t worker_stage;
  volatile bool worker_stage_valid;
  solar_os_flash_progress_stage_t last_stage;
  bool last_stage_valid;
} flash_app_state_t;

/* The only idle mutable storage is this pointer. The state itself is transient.
 */
static flash_app_state_t *flash_app;

static const char *flash_app_operation_name(flash_app_operation_t operation) {
  switch (operation) {
  case FLASH_APP_OPERATION_REFRESH:
    return "catalog refresh";
  case FLASH_APP_OPERATION_DOWNLOAD:
    return "artifact download";
  case FLASH_APP_OPERATION_PROGRAM:
    return "program";
  case FLASH_APP_OPERATION_NONE:
  default:
    return "operation";
  }
}

static solar_os_shell_io_t *flash_io(flash_app_state_t *state) {
  solar_os_shell_io_t *io = solar_os_context_shell_io(state->ctx);
  if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
    solar_os_shell_io_init_terminal(&state->fallback_io,
                                    solar_os_context_terminal(state->ctx));
    solar_os_context_set_shell_io(state->ctx, &state->fallback_io);
    io = &state->fallback_io;
  }
  return io;
}

static void flash_app_finish_line(flash_app_state_t *state) {
  solar_os_shell_io_t *io = flash_io(state);
  solar_os_shell_io_printf(io, "%s exits\n",
                           solar_os_shell_io_app_exit_key(io));
  solar_os_shell_io_flush(io);
}

static void flash_app_render(flash_app_state_t *state) {
  solar_os_shell_io_t *io = flash_io(state);
  solar_os_shell_io_clear(io);
  solar_os_shell_io_printf_bold(io, "Flash another ESP board\n");
  if (state->message[0] != '\0') {
    solar_os_shell_io_printf(io, "\n%s\n", state->message);
  }
  if (!solar_os_storage_sd_is_mounted()) {
    solar_os_shell_io_writeln(io, "SD card is not mounted.");
    flash_app_finish_line(state);
    return;
  }
  if (state->catalog == NULL) {
    solar_os_shell_io_writeln(io, "No verified catalog is cached.");
    solar_os_shell_io_writeln(io, "r refreshes the catalog");
    flash_app_finish_line(state);
    return;
  }
  solar_os_shell_io_printf(io, "%u artifacts; * cached on SD\n\n",
                           (unsigned)state->catalog->count);
  const size_t count = state->catalog->count;
  if (count == 0U) {
    solar_os_shell_io_writeln(io, "The catalog is empty.");
  }
  const size_t visible = 12U;
  size_t top = state->cursor >= visible ? state->cursor - visible + 1U : 0U;
  if (top + visible > count && count > visible)
    top = count - visible;
  for (size_t i = top; i < count && i < top + visible; i++) {
    const solar_os_flash_artifact_t *artifact = &state->catalog->artifacts[i];
    solar_os_shell_io_printf(
        io, "%c%c %-22s %-12s %-10s %s\n", i == state->cursor ? '>' : ' ',
        artifact->cached ? '*' : ' ', artifact->board_id, artifact->flavor,
        artifact->version, artifact->chip);
  }
  solar_os_shell_io_writeln(
      io, "\nup/down select  r refresh  d download  f flash via uart0");
  solar_os_shell_io_writeln(
      io, "For automatic boot/reset pins, use the shell command form.");
  flash_app_finish_line(state);
}

static void flash_app_progress(const solar_os_flash_progress_t *progress,
                               void *user) {
  flash_app_state_t *state = (flash_app_state_t *)user;
  if (state == NULL || state->events == NULL || progress == NULL) {
    return;
  }
  state->worker_stage = progress->stage;
  state->worker_stage_valid = true;
  const flash_app_event_t event = {
      .type = FLASH_APP_EVENT_PROGRESS,
      .progress = *progress,
  };
  (void)xQueueSend(state->events, &event, 0);
}

static void flash_app_worker(void *parameter) {
  flash_app_state_t *state = (flash_app_state_t *)parameter;
  esp_err_t result = ESP_ERR_INVALID_STATE;
  switch (state->operation) {
  case FLASH_APP_OPERATION_REFRESH:
    result = solar_os_flash_catalog_refresh(flash_app_progress, state);
    break;
  case FLASH_APP_OPERATION_DOWNLOAD:
    result = solar_os_flash_artifact_download(state->catalog, &state->artifact,
                                              flash_app_progress, state);
    break;
  case FLASH_APP_OPERATION_PROGRAM:
    result = solar_os_flash_artifact_program(&state->artifact, &state->program,
                                             flash_app_progress, state);
    break;
  case FLASH_APP_OPERATION_NONE:
  default:
    break;
  }
  const flash_app_event_t event = {
      .type = FLASH_APP_EVENT_DONE,
      .result = result,
  };
  (void)xQueueSend(state->events, &event, pdMS_TO_TICKS(100));
  state->task_done = true;
  solar_os_task_delete_external(NULL);
}

static bool flash_app_start_worker(flash_app_state_t *state,
                                   flash_app_operation_t operation) {
  if (state->running) {
    return false;
  }
  if (state->events == NULL) {
    state->events = solar_os_queue_create(FLASH_APP_EVENT_QUEUE_LEN,
                                          sizeof(flash_app_event_t));
    if (state->events == NULL)
      return false;
  } else {
    xQueueReset(state->events);
  }
  state->operation = operation;
  state->task_done = false;
  state->running = true;
  state->last_stage_valid = false;
  state->worker_stage_valid = false;
  state->result_received = false;
  state->result_screen = false;
  state->message[0] = '\0';
  SOLAR_OS_LOGI(TAG, "%s started", flash_app_operation_name(operation));
  const BaseType_t created = solar_os_task_create_pinned_external(
      flash_app_worker, "solar_os_flash", FLASH_APP_TASK_STACK, state,
      FLASH_APP_TASK_PRIORITY, &state->task, tskNO_AFFINITY,
      SOLAR_OS_TASK_ROLE_FOREGROUND);
  if (created != pdPASS) {
    state->running = false;
    state->task = NULL;
    return false;
  }
  return true;
}

static void
flash_app_print_progress(flash_app_state_t *state,
                         const solar_os_flash_progress_t *progress) {
  solar_os_shell_io_t *io = flash_io(state);
  if (!state->last_stage_valid || state->last_stage != progress->stage) {
    solar_os_shell_io_printf(
        io, "flash: %s", solar_os_flash_progress_stage_name(progress->stage));
    if (!progress->total_known)
      solar_os_shell_io_put_char(io, '\n');
    state->last_stage = progress->stage;
    state->last_stage_valid = true;
  }
  if (progress->total_known) {
    const unsigned percent =
        progress->bytes_total > 0U
            ? (unsigned)(((uint64_t)progress->bytes_done * 100U) /
                         progress->bytes_total)
            : 100U;
    solar_os_shell_io_printf(io, " %u%% (%u/%u)\n", percent,
                             (unsigned)progress->bytes_done,
                             (unsigned)progress->bytes_total);
  }
  solar_os_shell_io_flush(io);
}

static void flash_app_reload_catalog(flash_app_state_t *state) {
  solar_os_flash_catalog_t *catalog = NULL;
  const esp_err_t err = solar_os_flash_catalog_load(&catalog);
  solar_os_flash_catalog_free(state->catalog);
  state->catalog = err == ESP_OK ? catalog : NULL;
  if (state->catalog != NULL && state->cursor >= state->catalog->count) {
    state->cursor =
        state->catalog->count > 0U ? state->catalog->count - 1U : 0U;
  }
}

static void flash_app_drain_events(flash_app_state_t *state) {
  if (state->events == NULL)
    return;
  flash_app_event_t event;
  while (xQueueReceive(state->events, &event, 0) == pdPASS) {
    if (event.type == FLASH_APP_EVENT_PROGRESS) {
      flash_app_print_progress(state, &event.progress);
      continue;
    }
    state->result_received = true;
    solar_os_shell_io_t *io = flash_io(state);
    const char *operation = flash_app_operation_name(state->operation);
    const char *stage =
        state->worker_stage_valid
            ? solar_os_flash_progress_stage_name(state->worker_stage)
            : "startup";
    if (event.result == ESP_OK) {
      snprintf(state->message, sizeof(state->message), "%s succeeded",
               operation);
      SOLAR_OS_LOGI(TAG, "%s succeeded", operation);
      solar_os_shell_io_writeln(io, "flash: success");
      if (state->operation == FLASH_APP_OPERATION_REFRESH ||
          state->operation == FLASH_APP_OPERATION_DOWNLOAD) {
        flash_app_reload_catalog(state);
      }
    } else {
      snprintf(state->message, sizeof(state->message),
               "%s failed at %s: %s (0x%x)", operation, stage,
               esp_err_to_name(event.result), (unsigned)event.result);
      SOLAR_OS_LOGE(TAG, "%s failed stage=%s error=%s (0x%x)", operation, stage,
                    esp_err_to_name(event.result), (unsigned)event.result);
      solar_os_shell_io_printf(io, "flash: %s\n", state->message);
    }
    solar_os_shell_io_flush(io);
  }
  if (state->task_done && state->running) {
    state->running = false;
    if (!state->result_received) {
      strlcpy(state->message, "operation ended without a result event",
              sizeof(state->message));
      SOLAR_OS_LOGE(TAG, "%s", state->message);
      solar_os_shell_io_writeln(
          flash_io(state), "flash: operation ended without a result event");
    }
    if (state->command_mode) {
      state->command_exit_requested = true;
    } else {
      state->result_screen = true;
      solar_os_shell_io_writeln(flash_io(state),
                                "Press a key to return to the catalog; "
                                "app-exit exits.");
      solar_os_shell_io_flush(flash_io(state));
    }
  }
}

static bool flash_parse_pin(const char *text, int *pin) {
  if (text == NULL || pin == NULL || text[0] == '\0')
    return false;
  char *end = NULL;
  errno = 0;
  const long value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value < 0 || value > 63) {
    return false;
  }
  *pin = (int)value;
  return true;
}

static bool flash_parse_baud(const char *text, uint32_t *baud_rate) {
  if (text == NULL || baud_rate == NULL || text[0] == '\0')
    return false;
  char *end = NULL;
  errno = 0;
  const unsigned long value = strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value < 9600U ||
      value > 2000000U) {
    return false;
  }
  *baud_rate = (uint32_t)value;
  return true;
}

static bool flash_app_select(flash_app_state_t *state, const char *board,
                             const char *flavor, const char *version) {
  const solar_os_flash_artifact_t *artifact =
      solar_os_flash_catalog_find(state->catalog, board, flavor, version);
  if (artifact == NULL)
    return false;
  state->artifact = *artifact;
  return true;
}

static bool flash_app_handle_command(flash_app_state_t *state) {
  solar_os_context_t *ctx = state->ctx;
  solar_os_shell_io_t *io = flash_io(state);
  const int argc = solar_os_context_argc(ctx);
  if (argc <= 1)
    return false;
  state->command_mode = true;

  const char *command = solar_os_context_argv(ctx, 1);
  if (strcmp(command, "refresh") == 0 && argc == 2) {
    solar_os_shell_io_writeln(io, "flash: refreshing signed catalog");
    if (!flash_app_start_worker(state, FLASH_APP_OPERATION_REFRESH)) {
      solar_os_shell_io_writeln(io, "flash: worker could not start");
      state->command_exit_requested = true;
    }
    return true;
  }
  if (strcmp(command, "list") == 0 && argc == 2) {
    if (state->catalog == NULL) {
      solar_os_shell_io_writeln(
          io, "flash: no verified catalog; run flash refresh");
    } else {
      for (size_t i = 0U; i < state->catalog->count; i++) {
        const solar_os_flash_artifact_t *artifact =
            &state->catalog->artifacts[i];
        solar_os_shell_io_printf(io, "%c %s %s %s %s\n",
                                 artifact->cached ? '*' : ' ',
                                 artifact->board_id, artifact->flavor,
                                 artifact->version, artifact->chip);
      }
    }
    state->command_exit_requested = true;
    return true;
  }
  if (strcmp(command, "download") == 0) {
    if ((argc != 4 && argc != 5) || state->catalog == NULL ||
        !flash_app_select(state, solar_os_context_argv(ctx, 2),
                          solar_os_context_argv(ctx, 3),
                          argc == 5 ? solar_os_context_argv(ctx, 4) : NULL)) {
      solar_os_shell_io_writeln(io,
                                "usage: flash download BOARD FLAVOR [VERSION]");
      solar_os_shell_io_writeln(
          io, "flash: artifact not found in the verified catalog");
      state->command_exit_requested = true;
    } else if (!flash_app_start_worker(state, FLASH_APP_OPERATION_DOWNLOAD)) {
      solar_os_shell_io_writeln(io, "flash: worker could not start");
      state->command_exit_requested = true;
    }
    return true;
  }

  if (argc < 3 || state->catalog == NULL) {
    solar_os_shell_io_writeln(
        io, "usage: flash BOARD FLAVOR [version=VERSION] [port=uart0] "
            "[boot=PIN] [reset=PIN] [baud=RATE]");
    if (state->catalog == NULL) {
      solar_os_shell_io_writeln(
          io, "flash: no verified catalog; run flash refresh");
    }
    state->command_exit_requested = true;
    return true;
  }
  const char *version = NULL;
  strlcpy(state->port, SOLAR_OS_UART_PORT_NAME, sizeof(state->port));
  state->program.boot_pin = -1;
  state->program.reset_pin = -1;
  state->program.baud_rate = 460800U;
  bool valid = true;
  for (int i = 3; i < argc && valid; i++) {
    const char *arg = solar_os_context_argv(ctx, i);
    if (strncmp(arg, "version=", 8U) == 0)
      version = arg + 8U;
    else if (strncmp(arg, "port=", 5U) == 0) {
      valid = strlcpy(state->port, arg + 5U, sizeof(state->port)) <
                  sizeof(state->port) &&
              state->port[0] != '\0';
    } else if (strncmp(arg, "boot=", 5U) == 0)
      valid = flash_parse_pin(arg + 5U, &state->program.boot_pin);
    else if (strncmp(arg, "reset=", 6U) == 0)
      valid = flash_parse_pin(arg + 6U, &state->program.reset_pin);
    else if (strncmp(arg, "baud=", 5U) == 0)
      valid = flash_parse_baud(arg + 5U, &state->program.baud_rate);
    else
      valid = false;
  }
  state->program.port = state->port;
  if (!valid || !flash_app_select(state, solar_os_context_argv(ctx, 1),
                                  solar_os_context_argv(ctx, 2), version)) {
    solar_os_shell_io_writeln(io,
                              "flash: invalid options or artifact not found");
    state->command_exit_requested = true;
  } else if (!state->artifact.cached) {
    solar_os_shell_io_writeln(
        io, "flash: selected artifact is not cached; use flash download first");
    state->command_exit_requested = true;
  } else {
    solar_os_shell_io_printf(io, "flash: %s/%s/%s via %s\n",
                             state->artifact.board_id, state->artifact.flavor,
                             state->artifact.version, state->port);
    if (state->program.boot_pin < 0 || state->program.reset_pin < 0) {
      solar_os_shell_io_writeln(io,
                                "flash: put the target in ROM download mode "
                                "now; connect TX to RX, RX to TX, and GND");
    }
    if (!flash_app_start_worker(state, FLASH_APP_OPERATION_PROGRAM)) {
      solar_os_shell_io_writeln(io, "flash: worker could not start");
      state->command_exit_requested = true;
    }
  }
  return true;
}

static void flash_app_cleanup(void) {
  if (flash_app == NULL)
    return;
  if (flash_app->events != NULL)
    solar_os_queue_delete(flash_app->events);
  solar_os_flash_catalog_free(flash_app->catalog);
  solar_os_memory_free(flash_app);
  flash_app = NULL;
}

static esp_err_t flash_app_start(solar_os_context_t *ctx) {
  if (flash_app != NULL) {
    if (!flash_app->task_done && flash_app->task != NULL) {
      return ESP_ERR_INVALID_STATE;
    }
    flash_app_cleanup();
  }
  flash_app = solar_os_memory_calloc(1U, sizeof(*flash_app),
                                     SOLAR_OS_MEMORY_TRANSIENT, "flash.app");
  if (flash_app == NULL)
    return ESP_ERR_NO_MEM;
  flash_app->ctx = ctx;
  flash_app_reload_catalog(flash_app);
  if (!flash_app_handle_command(flash_app)) {
    flash_app_render(flash_app);
  }
  solar_os_shell_io_flush(flash_io(flash_app));
  return ESP_OK;
}

static void flash_app_stop(solar_os_context_t *ctx) {
  (void)ctx;
  if (flash_app == NULL)
    return;
  if (flash_app->running &&
      !solar_os_task_wait_done(flash_app->task, &flash_app->task_done,
                               SOLAR_OS_TASK_STOP_WAIT_MS)) {
    return;
  }
  flash_app_cleanup();
}

static bool flash_app_event(solar_os_context_t *ctx,
                            const solar_os_event_t *event) {
  if (flash_app == NULL || event == NULL)
    return false;
  if (event->type == SOLAR_OS_EVENT_TICK) {
    flash_app_drain_events(flash_app);
    if (flash_app->command_exit_requested) {
      solar_os_context_request_exit(ctx);
    }
    return true;
  }
  if (event->type != SOLAR_OS_EVENT_CHAR)
    return false;
  const uint8_t ch = (uint8_t)event->data.ch;
  if (ch == SOLAR_OS_KEY_APP_EXIT) {
    if (flash_app->running) {
      solar_os_shell_io_writeln(
          flash_io(flash_app),
          "flash: operation is active; wait for its result");
      solar_os_shell_io_flush(flash_io(flash_app));
    } else {
      solar_os_context_request_exit(ctx);
    }
    return true;
  }
  if (flash_app->result_screen) {
    flash_app->result_screen = false;
    flash_app_render(flash_app);
    return true;
  }
  if (flash_app->running || flash_app->command_mode)
    return true;
  const size_t count =
      flash_app->catalog != NULL ? flash_app->catalog->count : 0U;
  if ((ch == SOLAR_OS_KEY_UP || ch == 'k') && flash_app->cursor > 0U) {
    flash_app->cursor--;
  } else if ((ch == SOLAR_OS_KEY_DOWN || ch == 'j') &&
             flash_app->cursor + 1U < count) {
    flash_app->cursor++;
  } else if (ch == 'r' || ch == 'R') {
    (void)flash_app_start_worker(flash_app, FLASH_APP_OPERATION_REFRESH);
    return true;
  } else if ((ch == 'd' || ch == 'D') && count > 0U) {
    flash_app->artifact = flash_app->catalog->artifacts[flash_app->cursor];
    (void)flash_app_start_worker(flash_app, FLASH_APP_OPERATION_DOWNLOAD);
    return true;
  } else if ((ch == 'f' || ch == 'F') && count > 0U) {
    flash_app->artifact = flash_app->catalog->artifacts[flash_app->cursor];
    if (!flash_app->artifact.cached) {
      solar_os_shell_io_writeln(flash_io(flash_app),
                                "flash: download this artifact first");
      return true;
    }
    strlcpy(flash_app->port, SOLAR_OS_UART_PORT_NAME, sizeof(flash_app->port));
    flash_app->program = (solar_os_flash_program_options_t){
        .port = flash_app->port,
        .boot_pin = -1,
        .reset_pin = -1,
        .baud_rate = 460800U,
    };
    solar_os_shell_io_writeln(
        flash_io(flash_app),
        "flash: put the target in ROM download mode; connecting via uart0");
    (void)flash_app_start_worker(flash_app, FLASH_APP_OPERATION_PROGRAM);
    return true;
  } else {
    return true;
  }
  flash_app_render(flash_app);
  return true;
}

const solar_os_app_t solar_os_flash_app = {
    .name = "flash",
    .summary = "download and flash SolarOS onto another ESP board",
    .start = flash_app_start,
    .stop = flash_app_stop,
    .event = flash_app_event,
    .worker_stack_bytes = FLASH_APP_TASK_STACK,
    .worker_stack_external = true,
};
