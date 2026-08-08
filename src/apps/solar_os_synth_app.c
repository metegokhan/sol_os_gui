#include "solar_os_synth_app.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "solar_os_audio.h"
#include "solar_os_gfx.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_synth_voice.h"

#define SYNTH_APP_OWNER "app:synth"
#define SYNTH_APP_PULSE_MS 220U
#define SYNTH_APP_STATUS_POLL_MS 250U
#define SYNTH_APP_HELD_MAX 16U
#define SYNTH_APP_OCTAVE_MIN 2
#define SYNTH_APP_OCTAVE_MAX 6
#define SYNTH_APP_VELOCITY_STEP 5
#define SYNTH_APP_VOLUME_STEP 5

typedef enum {
  SYNTH_CONTROL_WAVE = 0,
  SYNTH_CONTROL_VOLUME,
  SYNTH_CONTROL_ATTACK,
  SYNTH_CONTROL_DECAY,
  SYNTH_CONTROL_SUSTAIN,
  SYNTH_CONTROL_RELEASE,
  SYNTH_CONTROL_COUNT,
} synth_control_t;

typedef struct {
  bool active;
  solar_os_input_source_t source;
  uint16_t physical_key;
  uint16_t usage;
  uint32_t frequency_hz;
  uint32_t release_at_ms;
  uint8_t semitone;
} synth_held_note_t;

typedef struct {
  solar_os_synth_voice_config_t config;
  synth_control_t selected;
  synth_held_note_t held[SYNTH_APP_HELD_MAX];
  int octave;
  uint8_t velocity;
  uint8_t volume;
  esp_err_t last_error;
  size_t last_active_voices;
  uint32_t last_deadline_misses;
  uint32_t last_status_poll_ms;
  bool last_running;
  bool suspended;
} synth_app_state_t;

static synth_app_state_t synth_app;

static const uint16_t synth_envelope_values[] = {
    0,   5,   10,   20,   40,   80,   120,  180,  250,   350,
    500, 750, 1000, 1500, 2000, 3000, 5000, 7500, 10000,
};

static const uint16_t synth_note_frequencies_octave_4[] = {
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494, 523,
};

static const uint16_t synth_note_usages[] = {
    0x04U, /* A */
    0x1aU, /* W */
    0x16U, /* S */
    0x08U, /* E */
    0x07U, /* D */
    0x09U, /* F */
    0x17U, /* T */
    0x0aU, /* G */
    0x1cU, /* Y physical position; Z on a German keyboard */
    0x0bU, /* H */
    0x18U, /* U */
    0x0dU, /* J */
    0x0eU, /* K */
};

static uint32_t synth_now_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint32_t synth_note_frequency(uint8_t semitone) {
  uint32_t frequency = synth_note_frequencies_octave_4[semitone];
  if (synth_app.octave > 4) {
    frequency <<= (unsigned)(synth_app.octave - 4);
  } else if (synth_app.octave < 4) {
    frequency >>= (unsigned)(4 - synth_app.octave);
  }
  return frequency;
}

static int synth_semitone_for_usage(uint16_t usage) {
  for (size_t i = 0;
       i < sizeof(synth_note_usages) / sizeof(synth_note_usages[0]); i++) {
    if (synth_note_usages[i] == usage) {
      return (int)i;
    }
  }
  return -1;
}

static int synth_semitone_for_char(uint8_t key) {
  if (key >= 'A' && key <= 'Z') {
    key = (uint8_t)(key - 'A' + 'a');
  }
  switch (key) {
  case 'a':
    return 0;
  case 'w':
    return 1;
  case 's':
    return 2;
  case 'e':
    return 3;
  case 'd':
    return 4;
  case 'f':
    return 5;
  case 't':
    return 6;
  case 'g':
    return 7;
  case 'y':
  case 'z':
    return 8;
  case 'h':
    return 9;
  case 'u':
    return 10;
  case 'j':
    return 11;
  case 'k':
    return 12;
  default:
    return -1;
  }
}

static int synth_semitone_for_key(const solar_os_input_key_event_t *key) {
  const int by_usage = synth_semitone_for_usage(key->usage);
  return by_usage >= 0 ? by_usage : synth_semitone_for_char(key->key);
}

static bool synth_semitone_held(uint8_t semitone) {
  for (size_t i = 0; i < SYNTH_APP_HELD_MAX; i++) {
    if (synth_app.held[i].active && synth_app.held[i].semitone == semitone) {
      return true;
    }
  }
  return false;
}

static void synth_draw_wave_icon(
    solar_os_gfx_t *gfx, int x, int y, int width, int height,
    const solar_os_synth_voice_status_t *status) {
  const int left = x;
  const int right = x + width;
  const int top = y;
  const int middle = y + height / 2;
  const int bottom = y + height;

  if (status != NULL && status->pcm_sample_count > 1U &&
      status->pcm_waveform == synth_app.config.waveform) {
    int32_t scope_peak = status->pcm_max;
    const int32_t negative_peak = -(int32_t)status->pcm_min;
    if (negative_peak > scope_peak) {
      scope_peak = negative_peak;
    }
    if (scope_peak < 1) {
      scope_peak = 1;
    }
    const int scope_amplitude = height > 3 ? (height / 2) - 1 : 1;
    for (size_t i = 1; i < status->pcm_sample_count; i++) {
      const int x0 = left +
                     (int)((i - 1U) * (size_t)width /
                           (status->pcm_sample_count - 1U));
      const int x1 = left +
                     (int)(i * (size_t)width /
                           (status->pcm_sample_count - 1U));
      const int y0 = middle -
                     ((int32_t)status->pcm_samples[i - 1U] * scope_amplitude) /
                         scope_peak;
      const int y1 = middle -
                     ((int32_t)status->pcm_samples[i] * scope_amplitude) /
                         scope_peak;
      solar_os_gfx_line(gfx, x0, y0, x1, y1);
    }
    return;
  }

  switch (synth_app.config.waveform) {
  case SOLAR_OS_SYNTH_WAVE_SQUARE:
    solar_os_gfx_line(gfx, left, bottom, left + width / 4, bottom);
    solar_os_gfx_line(gfx, left + width / 4, bottom, left + width / 4, top);
    solar_os_gfx_line(gfx, left + width / 4, top, left + (3 * width) / 4, top);
    solar_os_gfx_line(gfx, left + (3 * width) / 4, top, left + (3 * width) / 4,
                      bottom);
    solar_os_gfx_line(gfx, left + (3 * width) / 4, bottom, right, bottom);
    break;
  case SOLAR_OS_SYNTH_WAVE_TRIANGLE:
    solar_os_gfx_line(gfx, left, middle, left + width / 4, top);
    solar_os_gfx_line(gfx, left + width / 4, top, left + (3 * width) / 4,
                      bottom);
    solar_os_gfx_line(gfx, left + (3 * width) / 4, bottom, right, middle);
    break;
  case SOLAR_OS_SYNTH_WAVE_SAW:
    solar_os_gfx_line(gfx, left, bottom, right - 1, top);
    solar_os_gfx_line(gfx, right - 1, top, right - 1, bottom);
    break;
  case SOLAR_OS_SYNTH_WAVE_NOISE: {
    const int points[] = {middle,  top,        bottom, middle - 4, bottom,
                          top + 3, middle + 2, top,    bottom};
    const size_t count = sizeof(points) / sizeof(points[0]);
    for (size_t i = 1; i < count; i++) {
      const int x0 = left + (int)((i - 1) * (size_t)width / (count - 1));
      const int x1 = left + (int)(i * (size_t)width / (count - 1));
      solar_os_gfx_line(gfx, x0, points[i - 1], x1, points[i]);
    }
    break;
  }
  default:
    break;
  }
}

static size_t synth_envelope_value_index(uint32_t value) {
  size_t best = 0;
  uint32_t best_distance = UINT32_MAX;
  for (size_t i = 0;
       i < sizeof(synth_envelope_values) / sizeof(synth_envelope_values[0]);
       i++) {
    const uint32_t candidate = synth_envelope_values[i];
    const uint32_t distance =
        candidate > value ? candidate - value : value - candidate;
    if (distance < best_distance) {
      best = i;
      best_distance = distance;
    }
  }
  return best;
}

static void synth_draw_knob(solar_os_gfx_t *gfx, int center_x, int center_y,
                            int radius, const char *label, const char *value,
                            unsigned position, bool selected) {
  static const int8_t indicator_x[] = {-7, -10, -10, -8, -4, 0,
                                       4,  8,   10,  10, 7};
  static const int8_t indicator_y[] = {7, 4, 0, -5, -9, -10, -9, -5, 0, 4, 7};
  if (position > 10U) {
    position = 10U;
  }

  solar_os_gfx_circle(gfx, center_x, center_y, radius);
  if (selected) {
    solar_os_gfx_circle(gfx, center_x, center_y, radius + 3);
  }
  const int scale = radius > 18 ? radius - 7 : radius / 2;
  solar_os_gfx_line(gfx, center_x, center_y,
                    center_x + indicator_x[position] * scale / 10,
                    center_y + indicator_y[position] * scale / 10);

  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  const int label_x = center_x - (int)solar_os_gfx_text_width(gfx, label) / 2;
  solar_os_gfx_text(gfx, label_x, center_y + radius + 15, label);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  const int value_x = center_x - (int)solar_os_gfx_text_width(gfx, value) / 2;
  solar_os_gfx_text(gfx, value_x, center_y + radius + 29, value);
}

static void synth_draw_envelope(solar_os_gfx_t *gfx, int x, int y, int width,
                                int height) {
  const int left = x + 10;
  const int right = x + width - 10;
  const int top = y + 23;
  const int bottom = y + height - 8;
  const int graph_width = right - left;
  const int graph_height = bottom - top;
  const uint32_t attack_weight =
      (uint32_t)synth_envelope_value_index(synth_app.config.attack_ms) + 1U;
  const uint32_t decay_weight =
      (uint32_t)synth_envelope_value_index(synth_app.config.decay_ms) + 1U;
  const uint32_t sustain_weight = 8U;
  const uint32_t release_weight =
      (uint32_t)synth_envelope_value_index(synth_app.config.release_ms) + 1U;
  const uint32_t total_weight =
      attack_weight + decay_weight + sustain_weight + release_weight;
  const int attack_x =
      left + (int)((uint32_t)graph_width * attack_weight / total_weight);
  const int decay_x =
      left + (int)((uint32_t)graph_width *
                    (attack_weight + decay_weight) / total_weight);
  const int sustain_x =
      left + (int)((uint32_t)graph_width *
                    (attack_weight + decay_weight + sustain_weight) /
                    total_weight);
  const int sustain_y =
      bottom - graph_height * (int)synth_app.config.sustain_percent / 100;

  solar_os_gfx_rect(gfx, x, y, width, height);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  solar_os_gfx_text(gfx, x + 8, y + 16, "ENVELOPE");
  solar_os_gfx_line(gfx, left, bottom, attack_x, top);
  solar_os_gfx_line(gfx, attack_x, top, decay_x, sustain_y);
  solar_os_gfx_line(gfx, decay_x, sustain_y, sustain_x, sustain_y);
  solar_os_gfx_line(gfx, sustain_x, sustain_y, right, bottom);
}

static void synth_draw_volume_button(solar_os_gfx_t *gfx, int x, int y,
                                     int width, int height, bool selected) {
  char value[12];
  snprintf(value, sizeof(value), "%u%%", (unsigned)synth_app.volume);
  solar_os_gfx_rect(gfx, x, y, width, height);
  if (selected) {
    solar_os_gfx_rect(gfx, x + 3, y + 3, width - 6, height - 6);
  }
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  int text_x = x + (width - (int)solar_os_gfx_text_width(gfx, "VOLUME")) / 2;
  solar_os_gfx_text(gfx, text_x, y + 18, "VOLUME");
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  text_x = x + (width - (int)solar_os_gfx_text_width(gfx, value)) / 2;
  solar_os_gfx_text(gfx, text_x, y + 36, value);
}

static void synth_draw_piano(solar_os_gfx_t *gfx, int x, int y, int width,
                             int height) {
  static const uint8_t white_semitones[] = {0, 2, 4, 5, 7, 9, 11, 12};
  static const char *const white_labels[] = {"A", "S", "D", "F",
                                             "G", "H", "J", "K"};
  static const int8_t black_after_white[] = {0, 1, -1, 2, 3, 4, -1};
  static const uint8_t black_semitones[] = {1, 3, 6, 8, 10};
  static const char *const black_labels[] = {"W", "E", "T", "Y/Z", "U"};
  const int white_width = width / 8;

  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  for (size_t i = 0; i < 8; i++) {
    const int key_x = x + (int)i * white_width;
    const int key_width = i == 7 ? x + width - key_x : white_width;
    if (synth_semitone_held(white_semitones[i])) {
      solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
      solar_os_gfx_fill_rect(gfx, key_x + 1, y + 1, key_width - 1, height - 1);
    }
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, key_x, y, key_width, height);
  }

  for (size_t i = 0; i < sizeof(black_after_white); i++) {
    if (black_after_white[i] < 0) {
      continue;
    }
    const int black_index = black_after_white[i];
    const int key_x = x + ((int)i + 1) * white_width - white_width / 4;
    const int key_width = white_width / 2;
    const int key_height = (height * 3) / 5;
    const bool held = synth_semitone_held(black_semitones[black_index]);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, key_x, y, key_width, key_height);
    if (held) {
      solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
      solar_os_gfx_fill_rect(gfx, key_x + 3, y + 3, key_width - 6,
                             key_height - 6);
      solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
      solar_os_gfx_rect(gfx, key_x + 3, y + 3, key_width - 6, key_height - 6);
    }
    solar_os_gfx_set_color(gfx, held ? SOLAR_OS_GFX_COLOR_BLACK
                                     : SOLAR_OS_GFX_COLOR_WHITE);
    const char *label = black_labels[black_index];
    const int label_x =
        key_x + (key_width - (int)solar_os_gfx_text_width(gfx, label)) / 2;
    solar_os_gfx_text(gfx, label_x, y + key_height - 5, label);
  }

  solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
  for (size_t i = 0; i < 8; i++) {
    const int key_x = x + (int)i * white_width;
    const int key_width = i == 7 ? x + width - key_x : white_width;
    const char *label = white_labels[i];
    const int label_x =
        key_x + (key_width - (int)solar_os_gfx_text_width(gfx, label)) / 2;
    solar_os_gfx_text(gfx, label_x, y + height - 5, label);
  }
}

static void synth_render(solar_os_context_t *ctx) {
  solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
  if (gfx == NULL || synth_app.suspended) {
    return;
  }

  const int width = (int)solar_os_gfx_width(gfx);
  const int height = (int)solar_os_gfx_height(gfx);
  solar_os_synth_voice_status_t status;
  solar_os_synth_voice_get_status(&status);
  solar_os_audio_status_t audio_status;
  solar_os_audio_get_status(&audio_status);
  synth_app.volume = audio_status.volume;

  solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
  solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
  solar_os_gfx_text(gfx, 6, 18, "SYNTH");

  char header[96];
  const esp_err_t display_error =
      synth_app.last_error != ESP_OK ? synth_app.last_error : status.last_error;
  if (display_error != ESP_OK) {
    snprintf(header, sizeof(header), "audio: %s",
             esp_err_to_name(display_error));
  } else {
    snprintf(header, sizeof(header), "oct %d vel %u voices %u %uHz miss %u",
             synth_app.octave, (unsigned)synth_app.velocity,
             (unsigned)status.active_voices, (unsigned)status.sample_rate,
             (unsigned)status.render_deadline_misses);
  }
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  solar_os_gfx_text(gfx, 78, 17, header);

  const int graphs_top = 35;
  const int graphs_height = 70;
  const int piano_y = height - 99;
  const int wave_width = width / 4;
  const int wave_x = 6;
  const int wave_panel_width = wave_width - 10;
  solar_os_gfx_rect(gfx, wave_x, graphs_top, wave_panel_width, graphs_height);
  if (synth_app.selected == SYNTH_CONTROL_WAVE) {
    solar_os_gfx_rect(gfx, wave_x + 3, graphs_top + 3, wave_panel_width - 6,
                      graphs_height - 6);
  }
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  solar_os_gfx_text(gfx, wave_x + 8, graphs_top + 16, "WAVE");
  synth_draw_wave_icon(gfx, wave_x + 10, graphs_top + 25,
                       wave_panel_width - 20, 27, &status);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  char wave_status[24];
  const char *wave_name = "?";
  switch (synth_app.config.waveform) {
  case SOLAR_OS_SYNTH_WAVE_SQUARE:
    wave_name = "SQR";
    break;
  case SOLAR_OS_SYNTH_WAVE_TRIANGLE:
    wave_name = "TRI";
    break;
  case SOLAR_OS_SYNTH_WAVE_SAW:
    wave_name = "SAW";
    break;
  case SOLAR_OS_SYNTH_WAVE_NOISE:
    wave_name = "NOISE";
    break;
  default:
    break;
  }
  if (status.pcm_generation > 0U &&
      status.pcm_waveform == synth_app.config.waveform) {
    snprintf(wave_status, sizeof(wave_status), "%s %04lx", wave_name,
             (unsigned long)(status.pcm_hash & 0xffffU));
  } else {
    snprintf(wave_status, sizeof(wave_status), "%s", wave_name);
  }
  solar_os_gfx_text(gfx, wave_x + 8, graphs_top + graphs_height - 7,
                    wave_status);

  const int knob_area_x = wave_width;
  synth_draw_envelope(gfx, knob_area_x, graphs_top, width - knob_area_x - 6,
                      graphs_height);

  const int controls_top = graphs_top + graphs_height + 8;
  const int control_bottom = piano_y - 13;
  synth_draw_volume_button(gfx, wave_x, controls_top + 7, wave_panel_width, 48,
                           synth_app.selected == SYNTH_CONTROL_VOLUME);

  const int knob_cell = (width - knob_area_x) / 4;
  int knob_radius = knob_cell / 3;
  if (knob_radius > 22) {
    knob_radius = 22;
  } else if (knob_radius < 13) {
    knob_radius = 13;
  }
  const int knob_y = controls_top + knob_radius + 1;
  const uint32_t knob_values[] = {
      synth_app.config.attack_ms,
      synth_app.config.decay_ms,
      synth_app.config.sustain_percent,
      synth_app.config.release_ms,
  };
  const char *const knob_labels[] = {"A", "D", "S", "R"};
  for (size_t i = 0; i < 4; i++) {
    char value[16];
    unsigned position;
    if (i == 2) {
      snprintf(value, sizeof(value), "%u%%", (unsigned)knob_values[i]);
      position = (unsigned)knob_values[i] / 10U;
    } else {
      snprintf(value, sizeof(value), "%ums", (unsigned)knob_values[i]);
      const size_t index = synth_envelope_value_index(knob_values[i]);
      position =
          (unsigned)((index * 10U) / ((sizeof(synth_envelope_values) /
                                       sizeof(synth_envelope_values[0])) -
                                      1U));
    }
    synth_draw_knob(gfx, knob_area_x + (int)i * knob_cell + knob_cell / 2,
                    knob_y, knob_radius, knob_labels[i], value, position,
                    synth_app.selected ==
                        (synth_control_t)(i + SYNTH_CONTROL_ATTACK));
  }

  if (piano_y > control_bottom) {
    synth_draw_piano(gfx, 6, piano_y, width - 12, 72);
  }
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  solar_os_gfx_text(gfx, 6, height - 6,
                    "Arrows select/tune  PgUp/Dn octave  +/- velocity");
  solar_os_gfx_present(gfx);

  synth_app.last_active_voices = status.active_voices;
  synth_app.last_deadline_misses = status.render_deadline_misses;
  synth_app.last_running = status.running;
  synth_app.last_status_poll_ms = synth_now_ms();
}

static synth_held_note_t *synth_find_held(const solar_os_input_key_event_t *key,
                                          int semitone) {
  for (size_t i = 0; i < SYNTH_APP_HELD_MAX; i++) {
    synth_held_note_t *held = &synth_app.held[i];
    if (!held->active) {
      continue;
    }
    if (key->physical_key != SOLAR_OS_INPUT_PHYSICAL_NONE) {
      if (held->source == key->source &&
          held->physical_key == key->physical_key) {
        return held;
      }
    } else if (held->physical_key == SOLAR_OS_INPUT_PHYSICAL_NONE &&
               held->semitone == semitone) {
      return held;
    }
  }
  return NULL;
}

static synth_held_note_t *synth_allocate_held(void) {
  for (size_t i = 0; i < SYNTH_APP_HELD_MAX; i++) {
    if (!synth_app.held[i].active) {
      return &synth_app.held[i];
    }
  }
  return NULL;
}

static bool synth_note_on(const solar_os_input_key_event_t *key, int semitone) {
  synth_held_note_t *held = synth_find_held(key, semitone);
  if (held != NULL && key->physical_key != SOLAR_OS_INPUT_PHYSICAL_NONE) {
    return false;
  }
  if (held == NULL) {
    held = synth_allocate_held();
  }
  if (held == NULL) {
    return false;
  }

  const uint32_t frequency = synth_note_frequency((uint8_t)semitone);
  esp_err_t err =
      solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
  if (err == ESP_OK) {
    err = solar_os_synth_voice_note_on(SYNTH_APP_OWNER, frequency,
                                       synth_app.velocity);
  }
  synth_app.last_error = err;
  if (err != ESP_OK) {
    return true;
  }

  *held = (synth_held_note_t){
      .active = true,
      .source = key->source,
      .physical_key = key->physical_key,
      .usage = key->usage,
      .frequency_hz = frequency,
      .release_at_ms = key->physical_key == SOLAR_OS_INPUT_PHYSICAL_NONE
                           ? synth_now_ms() + SYNTH_APP_PULSE_MS
                           : 0,
      .semitone = (uint8_t)semitone,
  };
  return true;
}

static bool synth_release_held(synth_held_note_t *held) {
  if (held == NULL || !held->active) {
    return false;
  }
  const uint32_t frequency = held->frequency_hz;
  memset(held, 0, sizeof(*held));
  for (size_t i = 0; i < SYNTH_APP_HELD_MAX; i++) {
    if (synth_app.held[i].active &&
        synth_app.held[i].frequency_hz == frequency) {
      return true;
    }
  }
  const esp_err_t err =
      solar_os_synth_voice_note_off(SYNTH_APP_OWNER, frequency);
  if (err != ESP_OK) {
    synth_app.last_error = err;
  }
  return true;
}

static bool synth_note_off(const solar_os_input_key_event_t *key,
                           int semitone) {
  return synth_release_held(synth_find_held(key, semitone));
}

static void synth_release_all(bool stop) {
  if (stop) {
    (void)solar_os_synth_voice_stop(SYNTH_APP_OWNER);
  } else {
    (void)solar_os_synth_voice_all_notes_off(SYNTH_APP_OWNER);
  }
  memset(synth_app.held, 0, sizeof(synth_app.held));
}

static uint32_t *synth_selected_envelope_value(void) {
  switch (synth_app.selected) {
  case SYNTH_CONTROL_ATTACK:
    return &synth_app.config.attack_ms;
  case SYNTH_CONTROL_DECAY:
    return &synth_app.config.decay_ms;
  case SYNTH_CONTROL_RELEASE:
    return &synth_app.config.release_ms;
  default:
    return NULL;
  }
}

static void synth_adjust_selected(int direction) {
  if (synth_app.selected == SYNTH_CONTROL_WAVE) {
    int waveform = (int)synth_app.config.waveform + direction;
    if (waveform < SOLAR_OS_SYNTH_WAVE_SQUARE) {
      waveform = SOLAR_OS_SYNTH_WAVE_NOISE;
    } else if (waveform > SOLAR_OS_SYNTH_WAVE_NOISE) {
      waveform = SOLAR_OS_SYNTH_WAVE_SQUARE;
    }
    synth_app.config.waveform = (solar_os_synth_waveform_t)waveform;
  } else if (synth_app.selected == SYNTH_CONTROL_VOLUME) {
    int volume = (int)synth_app.volume + direction * SYNTH_APP_VOLUME_STEP;
    if (volume < 0) {
      volume = 0;
    } else if (volume > 100) {
      volume = 100;
    }
    synth_app.last_error = solar_os_audio_set_volume((uint8_t)volume);
    if (synth_app.last_error == ESP_OK) {
      synth_app.volume = (uint8_t)volume;
    }
    return;
  } else if (synth_app.selected == SYNTH_CONTROL_SUSTAIN) {
    int sustain = (int)synth_app.config.sustain_percent + direction * 5;
    if (sustain < 0) {
      sustain = 0;
    } else if (sustain > 100) {
      sustain = 100;
    }
    synth_app.config.sustain_percent = (uint8_t)sustain;
  } else {
    uint32_t *value = synth_selected_envelope_value();
    if (value != NULL) {
      size_t index = synth_envelope_value_index(*value);
      const size_t count =
          sizeof(synth_envelope_values) / sizeof(synth_envelope_values[0]);
      if (direction > 0 && index + 1 < count) {
        index++;
      } else if (direction < 0 && index > 0) {
        index--;
      }
      *value = synth_envelope_values[index];
    }
  }
  synth_app.last_error =
      solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
}

static bool synth_handle_control(solar_os_context_t *ctx, uint8_t key) {
  switch (key) {
  case SOLAR_OS_KEY_APP_EXIT:
  case SOLAR_OS_KEY_ESCAPE:
    solar_os_context_request_exit(ctx);
    return true;
  case SOLAR_OS_KEY_LEFT:
  case SOLAR_OS_KEY_CTRL_LEFT:
  case SOLAR_OS_KEY_SHIFT_LEFT:
    synth_app.selected = synth_app.selected == 0
                             ? SYNTH_CONTROL_COUNT - 1
                             : (synth_control_t)(synth_app.selected - 1);
    return true;
  case SOLAR_OS_KEY_RIGHT:
  case SOLAR_OS_KEY_CTRL_RIGHT:
  case SOLAR_OS_KEY_SHIFT_RIGHT:
    synth_app.selected =
        (synth_control_t)((synth_app.selected + 1) % SYNTH_CONTROL_COUNT);
    return true;
  case SOLAR_OS_KEY_UP:
  case SOLAR_OS_KEY_CTRL_UP:
  case SOLAR_OS_KEY_SHIFT_UP:
    synth_adjust_selected(1);
    return true;
  case SOLAR_OS_KEY_DOWN:
  case SOLAR_OS_KEY_CTRL_DOWN:
  case SOLAR_OS_KEY_SHIFT_DOWN:
    synth_adjust_selected(-1);
    return true;
  case SOLAR_OS_KEY_PAGE_UP:
  case SOLAR_OS_KEY_SHIFT_PAGE_UP:
    if (synth_app.octave < SYNTH_APP_OCTAVE_MAX) {
      synth_app.octave++;
    }
    return true;
  case SOLAR_OS_KEY_PAGE_DOWN:
  case SOLAR_OS_KEY_SHIFT_PAGE_DOWN:
    if (synth_app.octave > SYNTH_APP_OCTAVE_MIN) {
      synth_app.octave--;
    }
    return true;
  case '+':
  case '=':
    if (synth_app.velocity <=
        SOLAR_OS_SYNTH_VOICE_VELOCITY_MAX - SYNTH_APP_VELOCITY_STEP) {
      synth_app.velocity += SYNTH_APP_VELOCITY_STEP;
    } else {
      synth_app.velocity = SOLAR_OS_SYNTH_VOICE_VELOCITY_MAX;
    }
    return true;
  case '-':
    if (synth_app.velocity > SYNTH_APP_VELOCITY_STEP) {
      synth_app.velocity -= SYNTH_APP_VELOCITY_STEP;
    } else {
      synth_app.velocity = 1;
    }
    return true;
  default:
    return false;
  }
}

static esp_err_t synth_start(solar_os_context_t *ctx) {
  if (solar_os_context_gfx(ctx) == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  memset(&synth_app, 0, sizeof(synth_app));
  synth_app.config = (solar_os_synth_voice_config_t){
      .waveform = SOLAR_OS_SYNTH_WAVE_SQUARE,
      .attack_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_ATTACK_MS,
      .decay_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_DECAY_MS,
      .sustain_percent = SOLAR_OS_SYNTH_VOICE_DEFAULT_SUSTAIN_PERCENT,
      .release_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_RELEASE_MS,
  };
  synth_app.octave = 4;
  synth_app.velocity = SOLAR_OS_SYNTH_VOICE_DEFAULT_VELOCITY;
  solar_os_audio_status_t audio_status;
  solar_os_audio_get_status(&audio_status);
  synth_app.volume = audio_status.volume;
  synth_app.last_error =
      solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
  solar_os_context_set_graphics_active(ctx, true);
  synth_render(ctx);
  return ESP_OK;
}

static void synth_stop(solar_os_context_t *ctx) {
  synth_release_all(true);
  synth_app.suspended = false;
  solar_os_context_set_graphics_active(ctx, false);
}

static void synth_suspend(solar_os_context_t *ctx) {
  synth_release_all(true);
  synth_app.suspended = true;
  solar_os_context_set_graphics_active(ctx, false);
}

static void synth_resume(solar_os_context_t *ctx) {
  synth_app.suspended = false;
  synth_app.last_error =
      solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
  solar_os_context_set_graphics_active(ctx, true);
  synth_render(ctx);
}

static bool synth_event(solar_os_context_t *ctx,
                        const solar_os_event_t *event) {
  if (event == NULL) {
    return false;
  }

  if (event->type == SOLAR_OS_EVENT_KEY) {
    const solar_os_input_key_event_t *key = &event->data.key;
    const int semitone = synth_semitone_for_key(key);
    bool changed = false;
    if (semitone >= 0) {
      if (key->action == SOLAR_OS_INPUT_KEY_PRESS) {
        changed = synth_note_on(key, semitone);
      } else if (key->action == SOLAR_OS_INPUT_KEY_RELEASE) {
        changed = synth_note_off(key, semitone);
      }
    } else if (key->action != SOLAR_OS_INPUT_KEY_RELEASE) {
      changed = synth_handle_control(ctx, key->key);
    }
    if (changed) {
      synth_render(ctx);
    }
    return true;
  }

  if (event->type == SOLAR_OS_EVENT_CHAR) {
    solar_os_input_key_event_t key = {
        .key = (uint8_t)event->data.ch,
        .action = SOLAR_OS_INPUT_KEY_PRESS,
    };
    const int semitone = synth_semitone_for_key(&key);
    const bool changed = semitone >= 0 ? synth_note_on(&key, semitone)
                                       : synth_handle_control(ctx, key.key);
    if (changed) {
      synth_render(ctx);
    }
    return true;
  }

  if (event->type == SOLAR_OS_EVENT_TICK) {
    bool changed = false;
    for (size_t i = 0; i < SYNTH_APP_HELD_MAX; i++) {
      synth_held_note_t *held = &synth_app.held[i];
      if (held->active && held->release_at_ms != 0 &&
          (int32_t)(event->data.tick_ms - held->release_at_ms) >= 0) {
        changed |= synth_release_held(held);
      }
    }

    if ((uint32_t)(event->data.tick_ms - synth_app.last_status_poll_ms) >=
        SYNTH_APP_STATUS_POLL_MS) {
      solar_os_synth_voice_status_t status;
      solar_os_synth_voice_get_status(&status);
      synth_app.last_status_poll_ms = event->data.tick_ms;
      if (status.active_voices != synth_app.last_active_voices ||
          status.render_deadline_misses != synth_app.last_deadline_misses ||
          status.running != synth_app.last_running) {
        changed = true;
      }
    }
    if (changed) {
      synth_render(ctx);
    }
    return true;
  }

  if (event->type == SOLAR_OS_EVENT_RESUME) {
    synth_resume(ctx);
    return true;
  }
  return false;
}

const solar_os_app_t solar_os_synth_app = {
    .name = "synth",
    .summary = "polyphonic synthesizer and sound lab",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE | SOLAR_OS_APP_FLAG_KEY_EVENTS,
    .start = synth_start,
    .suspend = synth_suspend,
    .resume = synth_resume,
    .stop = synth_stop,
    .event = synth_event,
    .tick_interval_ms = 25U,
    .tick_deadline_ms = 10U,
};
