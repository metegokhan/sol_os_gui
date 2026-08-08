#include "solar_os_synth_app.h"

#include <math.h>
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
#define SYNTH_APP_WAVE_STEP 1024
#define SYNTH_APP_WAVE_STEP_LARGE 4096
#define SYNTH_APP_BRUSH_MAX 16U
#define SYNTH_APP_DEFAULT_WAVE_STEPS 16U
#define SYNTH_APP_TWO_PI 6.28318530717958647692f
#define SYNTH_APP_PIANO_HEIGHT 63
#define SYNTH_APP_PIANO_BOTTOM_OFFSET 90

typedef enum {
  SYNTH_TAB_PLAY = 0,
  SYNTH_TAB_WAVE,
  SYNTH_TAB_FILTER,
  SYNTH_TAB_OSCILLATOR2,
  SYNTH_TAB_COUNT,
} synth_tab_t;

typedef enum {
  SYNTH_BASE_SQUARE = 0,
  SYNTH_BASE_TRIANGLE,
  SYNTH_BASE_SAW,
  SYNTH_BASE_SUPERSAW,
  SYNTH_BASE_SINE,
  SYNTH_BASE_FLAT,
  SYNTH_BASE_COUNT,
} synth_wave_baseline_t;

typedef enum {
  SYNTH_CONTROL_WAVE = 0,
  SYNTH_CONTROL_VOLUME,
  SYNTH_CONTROL_ATTACK,
  SYNTH_CONTROL_DECAY,
  SYNTH_CONTROL_SUSTAIN,
  SYNTH_CONTROL_RELEASE,
  SYNTH_CONTROL_COUNT,
} synth_control_t;

typedef enum {
  SYNTH_FILTER_CONTROL_CUTOFF = 0,
  SYNTH_FILTER_CONTROL_RESONANCE,
  SYNTH_FILTER_CONTROL_AMOUNT,
  SYNTH_FILTER_CONTROL_ATTACK,
  SYNTH_FILTER_CONTROL_DECAY,
  SYNTH_FILTER_CONTROL_SUSTAIN,
  SYNTH_FILTER_CONTROL_RELEASE,
  SYNTH_FILTER_CONTROL_COUNT,
} synth_filter_control_t;

typedef enum {
  SYNTH_OSCILLATOR2_CONTROL_WAVE = 0,
  SYNTH_OSCILLATOR2_CONTROL_OCTAVE,
  SYNTH_OSCILLATOR2_CONTROL_DETUNE,
  SYNTH_OSCILLATOR2_CONTROL_MIX,
  SYNTH_OSCILLATOR2_CONTROL_COUNT,
} synth_oscillator2_control_t;

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
  synth_filter_control_t filter_selected;
  synth_oscillator2_control_t oscillator2_selected;
  synth_held_note_t held[SYNTH_APP_HELD_MAX];
  int octave;
  uint8_t velocity;
  uint8_t volume;
  synth_tab_t tab;
  synth_wave_baseline_t baseline;
  synth_wave_baseline_t baseline_undo;
  size_t wave_cursor;
  uint8_t wave_brush;
  size_t wave_steps;
  size_t wave_steps_undo;
  int16_t wavetable[SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES];
  int16_t wavetable_undo[SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES];
  bool wavetable_undo_valid;
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

static const uint16_t synth_filter_cutoff_values[] = {
    40,  60,   80,   120,  180,  250,  350,   500,  700,
    1000, 1500, 2200, 3200, 4800, 7000, 10000, 14000, 18000,
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

static const uint16_t synth_wavetable_step_counts[] = {16U, 32U, 64U};

static uint32_t synth_now_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static const char *synth_baseline_name(synth_wave_baseline_t baseline) {
  switch (baseline) {
  case SYNTH_BASE_SQUARE:
    return "SQUARE";
  case SYNTH_BASE_TRIANGLE:
    return "TRIANGLE";
  case SYNTH_BASE_SAW:
    return "SAW";
  case SYNTH_BASE_SUPERSAW:
    return "SUPERSAW";
  case SYNTH_BASE_SINE:
    return "SINE";
  case SYNTH_BASE_FLAT:
    return "FLAT";
  default:
    return "?";
  }
}

static const char *synth_wave_short_name(solar_os_synth_waveform_t waveform) {
  switch (waveform) {
  case SOLAR_OS_SYNTH_WAVE_SQUARE:
    return "SQR";
  case SOLAR_OS_SYNTH_WAVE_TRIANGLE:
    return "TRI";
  case SOLAR_OS_SYNTH_WAVE_SAW:
    return "SAW";
  case SOLAR_OS_SYNTH_WAVE_SINE:
    return "SINE";
  case SOLAR_OS_SYNTH_WAVE_NOISE:
    return "NOISE";
  case SOLAR_OS_SYNTH_WAVE_CUSTOM:
    return "USER";
  default:
    return "?";
  }
}

static int16_t synth_baseline_sample(synth_wave_baseline_t baseline,
                                     size_t index, size_t sample_count) {
  const uint32_t phase = (uint32_t)(((uint64_t)index << 32) / sample_count);
  switch (baseline) {
  case SYNTH_BASE_SQUARE:
    return index < sample_count / 2U ? -32767 : 32767;
  case SYNTH_BASE_TRIANGLE:
    return phase < 0x80000000U
               ? (int16_t)(-32767 + (int32_t)(phase >> 15))
               : (int16_t)(32767 - (int32_t)((phase - 0x80000000U) >> 15));
  case SYNTH_BASE_SAW:
    return (int16_t)((int32_t)(phase >> 16) - 32768);
  case SYNTH_BASE_SUPERSAW: {
    static const int32_t phase_offsets[] = {
        -0x0c000000, -0x08000000, -0x04000000, 0,
        0x04000000,  0x08000000,  0x0c000000,
    };
    int32_t accumulated = 0;
    for (size_t i = 0;
         i < sizeof(phase_offsets) / sizeof(phase_offsets[0]); i++) {
      const uint32_t shifted = phase + (uint32_t)phase_offsets[i];
      accumulated += (int32_t)(shifted >> 16) - 32768;
    }
    return (int16_t)(accumulated /
                     (int32_t)(sizeof(phase_offsets) /
                               sizeof(phase_offsets[0])));
  }
  case SYNTH_BASE_SINE:
    return (
        int16_t)(sinf(SYNTH_APP_TWO_PI * (float)index / (float)sample_count) *
                 32767.0f);
  case SYNTH_BASE_FLAT:
  default:
    return 0;
  }
}

static int16_t synth_wavetable_interpolate(const int16_t *table,
                                           size_t sample_count,
                                           size_t phase_numerator,
                                           size_t phase_denominator) {
  const size_t scaled = phase_numerator;
  const size_t index = (scaled / phase_denominator) % sample_count;
  const size_t next = (index + 1U) % sample_count;
  const int32_t fraction = (int32_t)(scaled % phase_denominator);
  const int32_t denominator = (int32_t)phase_denominator;
  return (int16_t)(((int32_t)table[index] * (denominator - fraction) +
                    (int32_t)table[next] * fraction) /
                   denominator);
}

static uint8_t synth_wavetable_brush_max(void) {
  size_t maximum = (synth_app.wave_steps - 1U) / 2U;
  if (maximum > SYNTH_APP_BRUSH_MAX) {
    maximum = SYNTH_APP_BRUSH_MAX;
  }
  return (uint8_t)maximum;
}

static void synth_wavetable_snapshot(void) {
  memcpy(synth_app.wavetable_undo, synth_app.wavetable,
         sizeof(synth_app.wavetable));
  synth_app.baseline_undo = synth_app.baseline;
  synth_app.wave_steps_undo = synth_app.wave_steps;
  synth_app.wavetable_undo_valid = true;
}

static esp_err_t synth_wavetable_upload(void) {
  int16_t playback_table[SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES];
  for (size_t i = 0; i < SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES; i++) {
    playback_table[i] = synth_wavetable_interpolate(
        synth_app.wavetable, synth_app.wave_steps, i * synth_app.wave_steps,
        SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES);
  }
  return solar_os_synth_voice_set_wavetable(
      SYNTH_APP_OWNER, playback_table, SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES);
}

static void synth_wavetable_commit(void) {
  esp_err_t err = synth_wavetable_upload();
  if (err == ESP_OK) {
    synth_app.config.waveform = SOLAR_OS_SYNTH_WAVE_CUSTOM;
    err = solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
  }
  synth_app.last_error = err;
}

static void synth_wavetable_fill(synth_wave_baseline_t baseline) {
  synth_app.baseline = baseline;
  memset(synth_app.wavetable, 0, sizeof(synth_app.wavetable));
  int32_t peak = 0;
  for (size_t i = 0; i < synth_app.wave_steps; i++) {
    synth_app.wavetable[i] =
        synth_baseline_sample(baseline, i, synth_app.wave_steps);
    int32_t magnitude = synth_app.wavetable[i];
    if (magnitude < 0) {
      magnitude = -magnitude;
    }
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  if (baseline == SYNTH_BASE_SUPERSAW && peak > 0 && peak < 32767) {
    for (size_t i = 0; i < synth_app.wave_steps; i++) {
      synth_app.wavetable[i] =
          (int16_t)(((int32_t)synth_app.wavetable[i] * 32767) / peak);
    }
  }
}

static void synth_wavetable_seed(synth_wave_baseline_t baseline,
                                 bool remember) {
  if (remember) {
    synth_wavetable_snapshot();
  }
  synth_wavetable_fill(baseline);
  synth_wavetable_commit();
}

static void synth_wavetable_draw(int direction, bool large_step) {
  synth_wavetable_snapshot();
  const int32_t delta = direction * (large_step ? SYNTH_APP_WAVE_STEP_LARGE
                                                : SYNTH_APP_WAVE_STEP);
  const int radius = synth_app.wave_brush;
  const int divisor = radius + 1;
  for (int offset = -radius; offset <= radius; offset++) {
    int index = (int)synth_app.wave_cursor + offset;
    while (index < 0) {
      index += (int)synth_app.wave_steps;
    }
    index %= (int)synth_app.wave_steps;
    const int weight = divisor - (offset < 0 ? -offset : offset);
    int32_t sample = synth_app.wavetable[index] + delta * weight / divisor;
    if (sample > 32767) {
      sample = 32767;
    } else if (sample < -32767) {
      sample = -32767;
    }
    synth_app.wavetable[index] = (int16_t)sample;
  }
  synth_wavetable_commit();
}

static void synth_wavetable_smooth(void) {
  synth_wavetable_snapshot();
  for (size_t i = 0; i < synth_app.wave_steps; i++) {
    const size_t previous =
        (i + synth_app.wave_steps - 1U) % synth_app.wave_steps;
    const size_t next = (i + 1U) % synth_app.wave_steps;
    synth_app.wavetable[i] =
        (int16_t)(((int32_t)synth_app.wavetable_undo[previous] +
                   synth_app.wavetable_undo[i] +
                   synth_app.wavetable_undo[next]) /
                  3);
  }
  synth_wavetable_commit();
}

static void synth_wavetable_normalize(void) {
  int32_t peak = 0;
  for (size_t i = 0; i < synth_app.wave_steps; i++) {
    int32_t magnitude = synth_app.wavetable[i];
    if (magnitude < 0) {
      magnitude = -magnitude;
    }
    if (magnitude > peak) {
      peak = magnitude;
    }
  }
  if (peak == 0 || peak == 32767) {
    return;
  }
  synth_wavetable_snapshot();
  for (size_t i = 0; i < synth_app.wave_steps; i++) {
    synth_app.wavetable[i] =
        (int16_t)(((int32_t)synth_app.wavetable[i] * 32767) / peak);
  }
  synth_wavetable_commit();
}

static void synth_wavetable_undo(void) {
  if (!synth_app.wavetable_undo_valid) {
    return;
  }
  for (size_t i = 0; i < SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES; i++) {
    const int16_t sample = synth_app.wavetable[i];
    synth_app.wavetable[i] = synth_app.wavetable_undo[i];
    synth_app.wavetable_undo[i] = sample;
  }
  const synth_wave_baseline_t baseline = synth_app.baseline;
  synth_app.baseline = synth_app.baseline_undo;
  synth_app.baseline_undo = baseline;
  const size_t old_steps = synth_app.wave_steps;
  synth_app.wave_steps = synth_app.wave_steps_undo;
  synth_app.wave_steps_undo = old_steps;
  synth_app.wave_cursor =
      synth_app.wave_cursor * synth_app.wave_steps / old_steps;
  if (synth_app.wave_cursor >= synth_app.wave_steps) {
    synth_app.wave_cursor = synth_app.wave_steps - 1U;
  }
  const uint8_t brush_max = synth_wavetable_brush_max();
  if (synth_app.wave_brush > brush_max) {
    synth_app.wave_brush = brush_max;
  }
  synth_wavetable_commit();
}

static void synth_wavetable_cycle_steps(void) {
  synth_wavetable_snapshot();
  const size_t old_steps = synth_app.wave_steps_undo;
  const size_t step_count = sizeof(synth_wavetable_step_counts) /
                            sizeof(synth_wavetable_step_counts[0]);
  size_t selected = 0;
  while (selected < step_count &&
         synth_wavetable_step_counts[selected] != old_steps) {
    selected++;
  }
  if (selected == step_count) {
    selected = step_count - 1U;
  }
  const size_t new_steps =
      synth_wavetable_step_counts[(selected + 1U) % step_count];
  memset(synth_app.wavetable, 0, sizeof(synth_app.wavetable));
  for (size_t i = 0; i < new_steps; i++) {
    synth_app.wavetable[i] = synth_wavetable_interpolate(
        synth_app.wavetable_undo, old_steps, i * old_steps, new_steps);
  }
  synth_app.wave_steps = new_steps;
  synth_app.wave_cursor = synth_app.wave_cursor * new_steps / old_steps;
  if (synth_app.wave_cursor >= new_steps) {
    synth_app.wave_cursor = new_steps - 1U;
  }
  const uint8_t brush_max = synth_wavetable_brush_max();
  if (synth_app.wave_brush > brush_max) {
    synth_app.wave_brush = brush_max;
  }
  synth_wavetable_commit();
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

static void synth_draw_wave_icon(solar_os_gfx_t *gfx, int x, int y, int width,
                                 int height,
                                 solar_os_synth_waveform_t waveform,
                                 const solar_os_synth_voice_status_t *status) {
  const int left = x;
  const int right = x + width;
  const int top = y;
  const int middle = y + height / 2;
  const int bottom = y + height;

  if (status != NULL && status->pcm_sample_count > 1U &&
      status->pcm_waveform == waveform) {
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
      const int x0 = left + (int)((i - 1U) * (size_t)width /
                                  (status->pcm_sample_count - 1U));
      const int x1 =
          left + (int)(i * (size_t)width / (status->pcm_sample_count - 1U));
      const int y0 =
          middle -
          ((int32_t)status->pcm_samples[i - 1U] * scope_amplitude) / scope_peak;
      const int y1 =
          middle -
          ((int32_t)status->pcm_samples[i] * scope_amplitude) / scope_peak;
      solar_os_gfx_line(gfx, x0, y0, x1, y1);
    }
    return;
  }

  switch (waveform) {
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
  case SOLAR_OS_SYNTH_WAVE_SINE: {
    int previous_x = left;
    int previous_y = middle;
    for (int column = 1; column <= width; column++) {
      const int point_x = left + column;
      const int point_y =
          middle -
          (int)(sinf(SYNTH_APP_TWO_PI * (float)column / (float)width) *
                (float)(height / 2));
      solar_os_gfx_line(gfx, previous_x, previous_y, point_x, point_y);
      previous_x = point_x;
      previous_y = point_y;
    }
    break;
  }
  case SOLAR_OS_SYNTH_WAVE_CUSTOM:
    for (int column = 1; column <= width; column++) {
      const int16_t previous = synth_wavetable_interpolate(
          synth_app.wavetable, synth_app.wave_steps,
          (size_t)(column - 1) * synth_app.wave_steps, (size_t)width);
      const int16_t current = synth_wavetable_interpolate(
          synth_app.wavetable, synth_app.wave_steps,
          (size_t)column * synth_app.wave_steps, (size_t)width);
      const int y0 = middle - (int32_t)previous * height / (2 * 32767);
      const int y1 = middle - (int32_t)current * height / (2 * 32767);
      solar_os_gfx_line(gfx, left + column - 1, y0, left + column, y1);
    }
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

static size_t synth_filter_cutoff_value_index(uint32_t value) {
  size_t best = 0;
  uint32_t best_distance = UINT32_MAX;
  for (size_t i = 0;
       i < sizeof(synth_filter_cutoff_values) /
               sizeof(synth_filter_cutoff_values[0]);
       i++) {
    const uint32_t candidate = synth_filter_cutoff_values[i];
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
                                int height, const char *title,
                                uint32_t attack_ms, uint32_t decay_ms,
                                uint8_t sustain_percent,
                                uint32_t release_ms) {
  const int left = x + 10;
  const int right = x + width - 10;
  const int top = y + 23;
  const int bottom = y + height - 8;
  const int graph_width = right - left;
  const int graph_height = bottom - top;
  const uint32_t attack_weight =
      (uint32_t)synth_envelope_value_index(attack_ms) + 1U;
  const uint32_t decay_weight =
      (uint32_t)synth_envelope_value_index(decay_ms) + 1U;
  const uint32_t sustain_weight = 8U;
  const uint32_t release_weight =
      (uint32_t)synth_envelope_value_index(release_ms) + 1U;
  const uint32_t total_weight =
      attack_weight + decay_weight + sustain_weight + release_weight;
  const int attack_x =
      left + (int)((uint32_t)graph_width * attack_weight / total_weight);
  const int decay_x =
      left + (int)((uint32_t)graph_width * (attack_weight + decay_weight) /
                   total_weight);
  const int sustain_x =
      left +
      (int)((uint32_t)graph_width *
            (attack_weight + decay_weight + sustain_weight) / total_weight);
  const int sustain_y =
      bottom - graph_height * (int)sustain_percent / 100;

  solar_os_gfx_rect(gfx, x, y, width, height);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  solar_os_gfx_text(gfx, x + 8, y + 16, title);
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

static void synth_draw_header(solar_os_gfx_t *gfx,
                              const solar_os_synth_voice_status_t *status) {
  const int width = (int)solar_os_gfx_width(gfx);
  solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_16);
  solar_os_gfx_text(gfx, 6, 18, "SYNTH");

  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  const bool compact = width < 380;
  const char *tabs = compact ? "P W F O2" : "PLAY WAVE FILT OSC2";
  if (compact) {
    if (synth_app.tab == SYNTH_TAB_PLAY) {
      tabs = "[P] W F O2";
    } else if (synth_app.tab == SYNTH_TAB_WAVE) {
      tabs = "P [W] F O2";
    } else if (synth_app.tab == SYNTH_TAB_FILTER) {
      tabs = "P W [F] O2";
    } else if (synth_app.tab == SYNTH_TAB_OSCILLATOR2) {
      tabs = "P W F [O2]";
    }
  } else if (synth_app.tab == SYNTH_TAB_PLAY) {
    tabs = "[PLAY] WAVE FILT OSC2";
  } else if (synth_app.tab == SYNTH_TAB_WAVE) {
    tabs = "PLAY [WAVE] FILT OSC2";
  } else if (synth_app.tab == SYNTH_TAB_FILTER) {
    tabs = "PLAY WAVE [FILT] OSC2";
  } else if (synth_app.tab == SYNTH_TAB_OSCILLATOR2) {
    tabs = "PLAY WAVE FILT [OSC2]";
  }
  solar_os_gfx_text(gfx, compact ? 64 : 62, 17, tabs);

  char header[64];
  const esp_err_t display_error = synth_app.last_error != ESP_OK
                                      ? synth_app.last_error
                                      : status->last_error;
  if (display_error != ESP_OK) {
    snprintf(header, sizeof(header), "audio: %s",
             esp_err_to_name(display_error));
  } else if (compact) {
    snprintf(header, sizeof(header), "o%d v%u %uv", synth_app.octave,
             (unsigned)synth_app.velocity, (unsigned)status->active_voices);
  } else {
    snprintf(header, sizeof(header), "o%d v%u %uv %uHz m%u", synth_app.octave,
             (unsigned)synth_app.velocity, (unsigned)status->active_voices,
             (unsigned)status->sample_rate,
             (unsigned)status->render_deadline_misses);
  }
  solar_os_gfx_text(gfx, compact ? 150 : 218, 17, header);
}

static void synth_draw_wave_editor(solar_os_gfx_t *gfx, int width, int height) {
  const int graph_x = 6;
  const int graph_y = 34;
  const int graph_width = width - 12;
  const int piano_y = height - SYNTH_APP_PIANO_BOTTOM_OFFSET;
  int graph_height = piano_y - graph_y - 22;
  if (graph_height < 80) {
    graph_height = 80;
  }
  const int left = graph_x + 8;
  const int right = graph_x + graph_width - 8;
  const int top = graph_y + 27;
  const int bottom = graph_y + graph_height - 10;
  const int middle = (top + bottom) / 2;
  const int amplitude = (bottom - top) / 2;

  solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
  solar_os_gfx_rect(gfx, graph_x, graph_y, graph_width, graph_height);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  solar_os_gfx_text(gfx, graph_x + 8, graph_y + 17, "WAVETABLE");
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  char graph_status[48];
  snprintf(graph_status, sizeof(graph_status), "%s b%u [ENTER %u]",
           synth_baseline_name(synth_app.baseline),
           (unsigned)synth_app.wave_brush, (unsigned)synth_app.wave_steps);
  const int status_x = graph_x + graph_width - 8 -
                       (int)solar_os_gfx_text_width(gfx, graph_status);
  solar_os_gfx_text(gfx, status_x, graph_y + 17, graph_status);
  solar_os_gfx_line(gfx, left, middle, right, middle);

  int previous_x = left;
  int previous_y = middle - (int32_t)synth_app.wavetable[0] * amplitude / 32767;
  for (size_t i = 1; i <= synth_app.wave_steps; i++) {
    const int x =
        left + (int)(i * (size_t)(right - left) / synth_app.wave_steps);
    const int y =
        middle - (int32_t)synth_app.wavetable[i % synth_app.wave_steps] *
                     amplitude / 32767;
    solar_os_gfx_line(gfx, previous_x, previous_y, x, y);
    previous_x = x;
    previous_y = y;
  }
  if (synth_app.wave_steps <= 64U) {
    for (size_t i = 0; i < synth_app.wave_steps; i++) {
      const int x = left + (int)(i * (size_t)(right - left) /
                                 synth_app.wave_steps);
      const int y =
          middle - (int32_t)synth_app.wavetable[i] * amplitude / 32767;
      solar_os_gfx_fill_rect(gfx, x - 1, y - 1, 3, 3);
    }
  }

  const int cursor_x =
      left + (int)(synth_app.wave_cursor * (size_t)(right - left) /
                   synth_app.wave_steps);
  const int cursor_y =
      middle -
      (int32_t)synth_app.wavetable[synth_app.wave_cursor] * amplitude / 32767;
  solar_os_gfx_line(gfx, cursor_x, top, cursor_x, bottom);
  solar_os_gfx_fill_rect(gfx, cursor_x - 2, cursor_y - 2, 5, 5);

  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  char editor_status[64];
  snprintf(editor_status, sizeof(editor_status),
           "L/R move U/D draw  %03u/%u %+6d b%u",
           (unsigned)synth_app.wave_cursor, (unsigned)synth_app.wave_steps,
           (int)synth_app.wavetable[synth_app.wave_cursor],
           (unsigned)synth_app.wave_brush);
  solar_os_gfx_text(gfx, 6, piano_y - 8, editor_status);

  synth_draw_piano(gfx, 6, piano_y, width - 12, SYNTH_APP_PIANO_HEIGHT);
  solar_os_gfx_text(gfx, 6, height - 6,
                    width >= 380
                        ? "1-4 tabs Enter steps B base R reset M smooth N norm"
                        : "Enter steps B base R reset M smooth N norm");
}

static void synth_draw_filter_response(solar_os_gfx_t *gfx, int x, int y,
                                       int width, int height) {
  const int left = x + 8;
  const int right = x + width - 8;
  const int top = y + 23;
  const int bottom = y + height - 8;
  const float cutoff = (float)synth_app.config.filter.cutoff_hz;
  const float resonance =
      (float)synth_app.config.filter.resonance_percent / 100.0f;
  const float quality = 0.5f + 9.5f * resonance * resonance;
  const float minimum_log =
      log10f((float)SOLAR_OS_SYNTH_VOICE_FILTER_CUTOFF_MIN_HZ);
  const float maximum_log =
      log10f((float)SOLAR_OS_SYNTH_VOICE_FILTER_CUTOFF_MAX_HZ);

  solar_os_gfx_rect(gfx, x, y, width, height);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  solar_os_gfx_text(gfx, x + 8, y + 16, "LOW-PASS");
  solar_os_gfx_line(gfx, left, bottom, right, bottom);

  int previous_x = left;
  int previous_y = top;
  for (int column = 0; column <= right - left; column++) {
    const float position = (float)column / (float)(right - left);
    const float frequency =
        powf(10.0f,
             minimum_log + position * (maximum_log - minimum_log));
    const float ratio = frequency / cutoff;
    const float squared = ratio * ratio;
    const float denominator = sqrtf((1.0f - squared) * (1.0f - squared) +
                                    squared / (quality * quality));
    const float magnitude = denominator > 0.0001f ? 1.0f / denominator : 10.0f;
    float db = 20.0f * log10f(magnitude);
    if (db > 12.0f) {
      db = 12.0f;
    } else if (db < -48.0f) {
      db = -48.0f;
    }
    const int point_x = left + column;
    const int point_y =
        top + (int)((12.0f - db) * (float)(bottom - top) / 60.0f);
    if (column > 0) {
      solar_os_gfx_line(gfx, previous_x, previous_y, point_x, point_y);
    }
    previous_x = point_x;
    previous_y = point_y;
  }
}

static void synth_format_frequency(char *value, size_t value_size,
                                   uint32_t frequency_hz) {
  if (frequency_hz >= 1000U && frequency_hz % 1000U == 0U) {
    snprintf(value, value_size, "%uk", (unsigned)(frequency_hz / 1000U));
  } else if (frequency_hz >= 1000U) {
    snprintf(value, value_size, "%.1fk", (double)frequency_hz / 1000.0);
  } else {
    snprintf(value, value_size, "%u", (unsigned)frequency_hz);
  }
}

static void synth_draw_filter_editor(solar_os_gfx_t *gfx, int width,
                                     int height) {
  const int graphs_top = 35;
  const bool compact = height < 280;
  const int graphs_height = compact ? 56 : 72;
  const int gap = 6;
  const int graph_width = (width - 18) / 2;
  synth_draw_filter_response(gfx, 6, graphs_top, graph_width, graphs_height);
  synth_draw_envelope(
      gfx, 12 + graph_width, graphs_top, width - graph_width - 18,
      graphs_height, "FILTER ENV", synth_app.config.filter.attack_ms,
      synth_app.config.filter.decay_ms,
      synth_app.config.filter.sustain_percent,
      synth_app.config.filter.release_ms);

  const int piano_y = height - SYNTH_APP_PIANO_BOTTOM_OFFSET;
  const int controls_top = graphs_top + graphs_height + gap;
  const int knob_cell = (width - 12) / SYNTH_FILTER_CONTROL_COUNT;
  int knob_radius = knob_cell / 3;
  const int maximum_radius = compact ? 10 : 16;
  if (knob_radius > maximum_radius) {
    knob_radius = maximum_radius;
  } else if (knob_radius < 10) {
    knob_radius = 10;
  }
  const int knob_y = controls_top + knob_radius;
  const char *const labels[] = {"CUT", "RES", "ENV", "A", "D", "S", "R"};
  const uint32_t values[] = {
      synth_app.config.filter.cutoff_hz,
      synth_app.config.filter.resonance_percent,
      synth_app.config.filter.envelope_amount_percent,
      synth_app.config.filter.attack_ms,
      synth_app.config.filter.decay_ms,
      synth_app.config.filter.sustain_percent,
      synth_app.config.filter.release_ms,
  };
  for (size_t i = 0; i < SYNTH_FILTER_CONTROL_COUNT; i++) {
    char value[12];
    unsigned position;
    if (i == SYNTH_FILTER_CONTROL_CUTOFF) {
      synth_format_frequency(value, sizeof(value), values[i]);
      const size_t index = synth_filter_cutoff_value_index(values[i]);
      position = (unsigned)(index * 10U /
                            ((sizeof(synth_filter_cutoff_values) /
                              sizeof(synth_filter_cutoff_values[0])) -
                             1U));
    } else if (i == SYNTH_FILTER_CONTROL_RESONANCE ||
               i == SYNTH_FILTER_CONTROL_AMOUNT ||
               i == SYNTH_FILTER_CONTROL_SUSTAIN) {
      snprintf(value, sizeof(value), "%u%%", (unsigned)values[i]);
      position = (unsigned)values[i] / 10U;
    } else {
      snprintf(value, sizeof(value), "%ums", (unsigned)values[i]);
      const size_t index = synth_envelope_value_index(values[i]);
      position = (unsigned)(index * 10U /
                            ((sizeof(synth_envelope_values) /
                              sizeof(synth_envelope_values[0])) -
                             1U));
    }
    synth_draw_knob(gfx, 6 + (int)i * knob_cell + knob_cell / 2, knob_y,
                    knob_radius, labels[i], value, position,
                    synth_app.filter_selected == (synth_filter_control_t)i);
  }

  synth_draw_piano(gfx, 6, piano_y, width - 12, SYNTH_APP_PIANO_HEIGHT);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  solar_os_gfx_text(gfx, 6, height - 6,
                    "Arrows select/tune  PgUp/Dn octave  +/- velocity");
}

static void synth_draw_oscillator_panel(
    solar_os_gfx_t *gfx, int x, int y, int width, int height, const char *title,
    solar_os_synth_waveform_t waveform, bool selected) {
  solar_os_gfx_rect(gfx, x, y, width, height);
  if (selected) {
    solar_os_gfx_rect(gfx, x + 3, y + 3, width - 6, height - 6);
  }
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD_12);
  solar_os_gfx_text(gfx, x + 8, y + 16, title);
  synth_draw_wave_icon(gfx, x + 10, y + 24, width - 20, height - 43,
                       waveform, NULL);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  solar_os_gfx_text(gfx, x + 8, y + height - 6,
                    synth_wave_short_name(waveform));
}

static void synth_draw_oscillator2_editor(solar_os_gfx_t *gfx, int width,
                                          int height) {
  const bool compact = height < 280;
  const int graphs_top = 35;
  const int graphs_height = compact ? 56 : 70;
  const int graph_gap = 6;
  const int graph_width = (width - 18) / 2;
  synth_draw_oscillator_panel(gfx, 6, graphs_top, graph_width, graphs_height,
                              "OSC1", synth_app.config.waveform, false);
  synth_draw_oscillator_panel(
      gfx, 12 + graph_width, graphs_top, width - graph_width - 18,
      graphs_height, "OSC2", synth_app.config.oscillator2.waveform,
      synth_app.oscillator2_selected == SYNTH_OSCILLATOR2_CONTROL_WAVE);

  const int piano_y = height - SYNTH_APP_PIANO_BOTTOM_OFFSET;
  const int controls_top = graphs_top + graphs_height + graph_gap;
  const int knob_cell = (width - 12) / SYNTH_OSCILLATOR2_CONTROL_COUNT;
  int knob_radius = knob_cell / 4;
  const int maximum_radius = compact ? 10 : 20;
  const int minimum_radius = compact ? 10 : 11;
  if (knob_radius > maximum_radius) {
    knob_radius = maximum_radius;
  } else if (knob_radius < minimum_radius) {
    knob_radius = minimum_radius;
  }
  const int knob_y = controls_top + knob_radius;
  const char *const labels[] = {"WAVE", "OCT", "FINE", "MIX"};
  char values[SYNTH_OSCILLATOR2_CONTROL_COUNT][12];
  snprintf(values[SYNTH_OSCILLATOR2_CONTROL_WAVE],
           sizeof(values[SYNTH_OSCILLATOR2_CONTROL_WAVE]), "%s",
           synth_wave_short_name(synth_app.config.oscillator2.waveform));
  snprintf(values[SYNTH_OSCILLATOR2_CONTROL_OCTAVE],
           sizeof(values[SYNTH_OSCILLATOR2_CONTROL_OCTAVE]), "%+d",
           (int)synth_app.config.oscillator2.octave);
  snprintf(values[SYNTH_OSCILLATOR2_CONTROL_DETUNE],
           sizeof(values[SYNTH_OSCILLATOR2_CONTROL_DETUNE]), "%+dc",
           (int)synth_app.config.oscillator2.detune_cents);
  snprintf(values[SYNTH_OSCILLATOR2_CONTROL_MIX],
           sizeof(values[SYNTH_OSCILLATOR2_CONTROL_MIX]), "%u%%",
           (unsigned)synth_app.config.oscillator2.mix_percent);
  const unsigned positions[] = {
      (unsigned)synth_app.config.oscillator2.waveform * 10U /
          SOLAR_OS_SYNTH_WAVE_CUSTOM,
      (unsigned)(synth_app.config.oscillator2.octave -
                 SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MIN) *
          10U /
          (SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MAX -
           SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MIN),
      (unsigned)(synth_app.config.oscillator2.detune_cents -
                 SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MIN_CENTS) *
          10U /
          (SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MAX_CENTS -
           SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MIN_CENTS),
      synth_app.config.oscillator2.mix_percent / 10U,
  };
  for (size_t i = 0; i < SYNTH_OSCILLATOR2_CONTROL_COUNT; i++) {
    synth_draw_knob(gfx, 6 + (int)i * knob_cell + knob_cell / 2, knob_y,
                    knob_radius, labels[i], values[i], positions[i],
                    synth_app.oscillator2_selected ==
                        (synth_oscillator2_control_t)i);
  }

  synth_draw_piano(gfx, 6, piano_y, width - 12, SYNTH_APP_PIANO_HEIGHT);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  solar_os_gfx_text(gfx, 6, height - 6,
                    "Arrows select/tune  PgUp/Dn octave  +/- velocity");
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
  synth_draw_header(gfx, &status);

  if (synth_app.tab == SYNTH_TAB_WAVE) {
    synth_draw_wave_editor(gfx, width, height);
    solar_os_gfx_present(gfx);
    synth_app.last_active_voices = status.active_voices;
    synth_app.last_deadline_misses = status.render_deadline_misses;
    synth_app.last_running = status.running;
    synth_app.last_status_poll_ms = synth_now_ms();
    return;
  }

  if (synth_app.tab == SYNTH_TAB_FILTER) {
    synth_draw_filter_editor(gfx, width, height);
    solar_os_gfx_present(gfx);
    synth_app.last_active_voices = status.active_voices;
    synth_app.last_deadline_misses = status.render_deadline_misses;
    synth_app.last_running = status.running;
    synth_app.last_status_poll_ms = synth_now_ms();
    return;
  }

  if (synth_app.tab == SYNTH_TAB_OSCILLATOR2) {
    synth_draw_oscillator2_editor(gfx, width, height);
    solar_os_gfx_present(gfx);
    synth_app.last_active_voices = status.active_voices;
    synth_app.last_deadline_misses = status.render_deadline_misses;
    synth_app.last_running = status.running;
    synth_app.last_status_poll_ms = synth_now_ms();
    return;
  }

  const int graphs_top = 35;
  const int graphs_height = 70;
  const int piano_y = height - SYNTH_APP_PIANO_BOTTOM_OFFSET;
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
  synth_draw_wave_icon(gfx, wave_x + 10, graphs_top + 25, wave_panel_width - 20,
                       27, synth_app.config.waveform, &status);
  solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_12);
  char wave_status[24];
  const char *wave_name = synth_wave_short_name(synth_app.config.waveform);
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
                      graphs_height, "ENVELOPE", synth_app.config.attack_ms,
                      synth_app.config.decay_ms,
                      synth_app.config.sustain_percent,
                      synth_app.config.release_ms);

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
    synth_draw_piano(gfx, 6, piano_y, width - 12,
                     SYNTH_APP_PIANO_HEIGHT);
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
  const esp_err_t err = solar_os_synth_voice_note_on(
      SYNTH_APP_OWNER, frequency, synth_app.velocity);
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
      waveform = SOLAR_OS_SYNTH_WAVE_CUSTOM;
    } else if (waveform > SOLAR_OS_SYNTH_WAVE_CUSTOM) {
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

static uint32_t *synth_selected_filter_envelope_value(void) {
  switch (synth_app.filter_selected) {
  case SYNTH_FILTER_CONTROL_ATTACK:
    return &synth_app.config.filter.attack_ms;
  case SYNTH_FILTER_CONTROL_DECAY:
    return &synth_app.config.filter.decay_ms;
  case SYNTH_FILTER_CONTROL_RELEASE:
    return &synth_app.config.filter.release_ms;
  default:
    return NULL;
  }
}

static void synth_adjust_filter_selected(int direction) {
  if (synth_app.filter_selected == SYNTH_FILTER_CONTROL_CUTOFF) {
    size_t index =
        synth_filter_cutoff_value_index(synth_app.config.filter.cutoff_hz);
    const size_t count = sizeof(synth_filter_cutoff_values) /
                         sizeof(synth_filter_cutoff_values[0]);
    if (direction > 0 && index + 1U < count) {
      index++;
    } else if (direction < 0 && index > 0U) {
      index--;
    }
    synth_app.config.filter.cutoff_hz = synth_filter_cutoff_values[index];
  } else if (synth_app.filter_selected == SYNTH_FILTER_CONTROL_RESONANCE) {
    int resonance = (int)synth_app.config.filter.resonance_percent +
                    direction * 5;
    if (resonance < 0) {
      resonance = 0;
    } else if (resonance > 100) {
      resonance = 100;
    }
    synth_app.config.filter.resonance_percent = (uint8_t)resonance;
  } else if (synth_app.filter_selected == SYNTH_FILTER_CONTROL_AMOUNT) {
    int amount = (int)synth_app.config.filter.envelope_amount_percent +
                 direction * 5;
    if (amount < 0) {
      amount = 0;
    } else if (amount > 100) {
      amount = 100;
    }
    synth_app.config.filter.envelope_amount_percent = (uint8_t)amount;
  } else if (synth_app.filter_selected == SYNTH_FILTER_CONTROL_SUSTAIN) {
    int sustain = (int)synth_app.config.filter.sustain_percent + direction * 5;
    if (sustain < 0) {
      sustain = 0;
    } else if (sustain > 100) {
      sustain = 100;
    }
    synth_app.config.filter.sustain_percent = (uint8_t)sustain;
  } else {
    uint32_t *value = synth_selected_filter_envelope_value();
    if (value != NULL) {
      size_t index = synth_envelope_value_index(*value);
      const size_t count =
          sizeof(synth_envelope_values) / sizeof(synth_envelope_values[0]);
      if (direction > 0 && index + 1U < count) {
        index++;
      } else if (direction < 0 && index > 0U) {
        index--;
      }
      *value = synth_envelope_values[index];
    }
  }
  synth_app.last_error =
      solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
}

static void synth_adjust_oscillator2_selected(int direction) {
  solar_os_synth_oscillator_config_t *oscillator2 =
      &synth_app.config.oscillator2;
  switch (synth_app.oscillator2_selected) {
  case SYNTH_OSCILLATOR2_CONTROL_WAVE: {
    int waveform = (int)oscillator2->waveform + direction;
    if (waveform < SOLAR_OS_SYNTH_WAVE_SQUARE) {
      waveform = SOLAR_OS_SYNTH_WAVE_CUSTOM;
    } else if (waveform > SOLAR_OS_SYNTH_WAVE_CUSTOM) {
      waveform = SOLAR_OS_SYNTH_WAVE_SQUARE;
    }
    oscillator2->waveform = (solar_os_synth_waveform_t)waveform;
    break;
  }
  case SYNTH_OSCILLATOR2_CONTROL_OCTAVE: {
    int octave = oscillator2->octave + direction;
    if (octave < SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MIN) {
      octave = SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MIN;
    } else if (octave > SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MAX) {
      octave = SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_OCTAVE_MAX;
    }
    oscillator2->octave = (int8_t)octave;
    break;
  }
  case SYNTH_OSCILLATOR2_CONTROL_DETUNE: {
    int detune = oscillator2->detune_cents + direction * 5;
    if (detune < SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MIN_CENTS) {
      detune = SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MIN_CENTS;
    } else if (detune > SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MAX_CENTS) {
      detune = SOLAR_OS_SYNTH_VOICE_OSCILLATOR2_DETUNE_MAX_CENTS;
    }
    oscillator2->detune_cents = (int16_t)detune;
    break;
  }
  case SYNTH_OSCILLATOR2_CONTROL_MIX: {
    int mix = oscillator2->mix_percent + direction * 5;
    if (mix < 0) {
      mix = 0;
    } else if (mix > 100) {
      mix = 100;
    }
    oscillator2->mix_percent = (uint8_t)mix;
    break;
  }
  default:
    break;
  }
  synth_app.last_error =
      solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
}

static void synth_move_wave_cursor(int direction, size_t step) {
  const size_t count = synth_app.wave_steps;
  if (step >= count) {
    step = count / 2U;
  }
  if (direction > 0) {
    synth_app.wave_cursor = (synth_app.wave_cursor + step) % count;
  } else {
    synth_app.wave_cursor =
        (synth_app.wave_cursor + count - (step % count)) % count;
  }
}

static bool synth_handle_wave_control(uint8_t key) {
  switch (key) {
  case SOLAR_OS_KEY_LEFT:
    synth_move_wave_cursor(-1, 1U);
    return true;
  case SOLAR_OS_KEY_RIGHT:
    synth_move_wave_cursor(1, 1U);
    return true;
  case SOLAR_OS_KEY_SHIFT_LEFT:
  case SOLAR_OS_KEY_CTRL_LEFT:
    synth_move_wave_cursor(-1, 8U);
    return true;
  case SOLAR_OS_KEY_SHIFT_RIGHT:
  case SOLAR_OS_KEY_CTRL_RIGHT:
    synth_move_wave_cursor(1, 8U);
    return true;
  case SOLAR_OS_KEY_CTRL_SHIFT_LEFT:
    synth_move_wave_cursor(-1, 32U);
    return true;
  case SOLAR_OS_KEY_CTRL_SHIFT_RIGHT:
    synth_move_wave_cursor(1, 32U);
    return true;
  case SOLAR_OS_KEY_UP:
  case SOLAR_OS_KEY_CTRL_UP:
    synth_wavetable_draw(1, false);
    return true;
  case SOLAR_OS_KEY_DOWN:
  case SOLAR_OS_KEY_CTRL_DOWN:
    synth_wavetable_draw(-1, false);
    return true;
  case SOLAR_OS_KEY_SHIFT_UP:
  case SOLAR_OS_KEY_CTRL_SHIFT_UP:
    synth_wavetable_draw(1, true);
    return true;
  case SOLAR_OS_KEY_SHIFT_DOWN:
  case SOLAR_OS_KEY_CTRL_SHIFT_DOWN:
    synth_wavetable_draw(-1, true);
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
  case SOLAR_OS_KEY_HOME:
  case SOLAR_OS_KEY_SHIFT_HOME:
  case SOLAR_OS_KEY_CTRL_HOME:
  case SOLAR_OS_KEY_CTRL_SHIFT_HOME:
    synth_app.wave_cursor = 0;
    return true;
  case SOLAR_OS_KEY_END:
  case SOLAR_OS_KEY_SHIFT_END:
  case SOLAR_OS_KEY_CTRL_END:
  case SOLAR_OS_KEY_CTRL_SHIFT_END:
    synth_app.wave_cursor = synth_app.wave_steps - 1U;
    return true;
  case SOLAR_OS_KEY_ENTER:
    synth_wavetable_cycle_steps();
    return true;
  case '+':
  case '=':
    if (synth_app.wave_brush < synth_wavetable_brush_max()) {
      synth_app.wave_brush++;
    }
    return true;
  case '-':
    if (synth_app.wave_brush > 0U) {
      synth_app.wave_brush--;
    }
    return true;
  case 'b':
  case 'B':
    synth_wavetable_seed(
        (synth_wave_baseline_t)((synth_app.baseline + 1) % SYNTH_BASE_COUNT),
        true);
    return true;
  case 'r':
  case 'R':
    synth_wavetable_seed(synth_app.baseline, true);
    return true;
  case 'm':
  case 'M':
    synth_wavetable_smooth();
    return true;
  case 'n':
  case 'N':
    synth_wavetable_normalize();
    return true;
  case '0':
    synth_wavetable_seed(SYNTH_BASE_FLAT, true);
    return true;
  case '\b':
  case 0x7fU:
  case SOLAR_OS_KEY_DELETE:
    synth_wavetable_undo();
    return true;
  default:
    return false;
  }
}

static bool synth_handle_filter_control(uint8_t key) {
  switch (key) {
  case SOLAR_OS_KEY_LEFT:
  case SOLAR_OS_KEY_CTRL_LEFT:
  case SOLAR_OS_KEY_SHIFT_LEFT:
    synth_app.filter_selected =
        synth_app.filter_selected == 0
            ? SYNTH_FILTER_CONTROL_COUNT - 1
            : (synth_filter_control_t)(synth_app.filter_selected - 1);
    return true;
  case SOLAR_OS_KEY_RIGHT:
  case SOLAR_OS_KEY_CTRL_RIGHT:
  case SOLAR_OS_KEY_SHIFT_RIGHT:
    synth_app.filter_selected = (synth_filter_control_t)(
        (synth_app.filter_selected + 1) % SYNTH_FILTER_CONTROL_COUNT);
    return true;
  case SOLAR_OS_KEY_UP:
  case SOLAR_OS_KEY_CTRL_UP:
  case SOLAR_OS_KEY_SHIFT_UP:
    synth_adjust_filter_selected(1);
    return true;
  case SOLAR_OS_KEY_DOWN:
  case SOLAR_OS_KEY_CTRL_DOWN:
  case SOLAR_OS_KEY_SHIFT_DOWN:
    synth_adjust_filter_selected(-1);
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

static bool synth_handle_oscillator2_control(uint8_t key) {
  switch (key) {
  case SOLAR_OS_KEY_LEFT:
  case SOLAR_OS_KEY_CTRL_LEFT:
  case SOLAR_OS_KEY_SHIFT_LEFT:
    synth_app.oscillator2_selected =
        synth_app.oscillator2_selected == 0
            ? SYNTH_OSCILLATOR2_CONTROL_COUNT - 1
            : (synth_oscillator2_control_t)(
                  synth_app.oscillator2_selected - 1);
    return true;
  case SOLAR_OS_KEY_RIGHT:
  case SOLAR_OS_KEY_CTRL_RIGHT:
  case SOLAR_OS_KEY_SHIFT_RIGHT:
    synth_app.oscillator2_selected = (synth_oscillator2_control_t)(
        (synth_app.oscillator2_selected + 1) %
        SYNTH_OSCILLATOR2_CONTROL_COUNT);
    return true;
  case SOLAR_OS_KEY_UP:
  case SOLAR_OS_KEY_CTRL_UP:
  case SOLAR_OS_KEY_SHIFT_UP:
    synth_adjust_oscillator2_selected(1);
    return true;
  case SOLAR_OS_KEY_DOWN:
  case SOLAR_OS_KEY_CTRL_DOWN:
  case SOLAR_OS_KEY_SHIFT_DOWN:
    synth_adjust_oscillator2_selected(-1);
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

static void synth_select_tab(synth_tab_t tab) {
  synth_app.tab = tab;
  if (synth_app.tab == SYNTH_TAB_WAVE) {
    synth_wavetable_commit();
  }
}

static bool synth_handle_control(solar_os_context_t *ctx, uint8_t key) {
  if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE) {
    solar_os_context_request_exit(ctx);
    return true;
  }
  if (key == '\t') {
    synth_select_tab(
        (synth_tab_t)((synth_app.tab + 1) % SYNTH_TAB_COUNT));
    return true;
  }
  if (key >= '1' && key <= '4') {
    synth_select_tab((synth_tab_t)(key - '1'));
    return true;
  }
  if (synth_app.tab == SYNTH_TAB_WAVE) {
    return synth_handle_wave_control(key);
  }
  if (synth_app.tab == SYNTH_TAB_FILTER) {
    return synth_handle_filter_control(key);
  }
  if (synth_app.tab == SYNTH_TAB_OSCILLATOR2) {
    return synth_handle_oscillator2_control(key);
  }

  switch (key) {
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
      .oscillator2 = {
          .waveform = SOLAR_OS_SYNTH_WAVE_SQUARE,
          .octave = SOLAR_OS_SYNTH_VOICE_DEFAULT_OSCILLATOR2_OCTAVE,
          .detune_cents =
              SOLAR_OS_SYNTH_VOICE_DEFAULT_OSCILLATOR2_DETUNE_CENTS,
          .mix_percent =
              SOLAR_OS_SYNTH_VOICE_DEFAULT_OSCILLATOR2_MIX_PERCENT,
      },
      .filter = {
          .cutoff_hz = SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_CUTOFF_HZ,
          .resonance_percent =
              SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_RESONANCE_PERCENT,
          .envelope_amount_percent =
              SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_ENVELOPE_AMOUNT_PERCENT,
          .attack_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_ATTACK_MS,
          .decay_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_DECAY_MS,
          .sustain_percent =
              SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_SUSTAIN_PERCENT,
          .release_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_FILTER_RELEASE_MS,
      },
  };
  synth_app.octave = 4;
  synth_app.velocity = SOLAR_OS_SYNTH_VOICE_DEFAULT_VELOCITY;
  synth_app.tab = SYNTH_TAB_PLAY;
  synth_app.baseline = SYNTH_BASE_SQUARE;
  synth_app.wave_steps = SYNTH_APP_DEFAULT_WAVE_STEPS;
  synth_app.wave_cursor = synth_app.wave_steps / 4U;
  synth_app.wave_brush = 1U;
  synth_wavetable_fill(synth_app.baseline);
  solar_os_audio_status_t audio_status;
  solar_os_audio_get_status(&audio_status);
  synth_app.volume = audio_status.volume;
  synth_app.last_error = synth_wavetable_upload();
  if (synth_app.last_error == ESP_OK) {
    synth_app.last_error =
        solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
  }
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
  synth_app.last_error = synth_wavetable_upload();
  if (synth_app.last_error == ESP_OK) {
    synth_app.last_error =
        solar_os_synth_voice_configure(SYNTH_APP_OWNER, &synth_app.config);
  }
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
    .summary = "polyphonic synthesizer and sound designer",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE | SOLAR_OS_APP_FLAG_KEY_EVENTS,
    .start = synth_start,
    .suspend = synth_suspend,
    .resume = synth_resume,
    .stop = synth_stop,
    .event = synth_event,
    .tick_interval_ms = 25U,
    .tick_deadline_ms = 10U,
};
