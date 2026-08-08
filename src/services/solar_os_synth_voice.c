#include "solar_os_synth_voice.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"

#define VOICE_BLOCK_FRAMES SOLAR_OS_SYNTH_BLOCK_FRAMES_DEFAULT
#define VOICE_OUTPUT_PEAK 12000
#define VOICE_OVERSAMPLE 8U
#define ENVELOPE_MAX 65535U
#define NOISE_SEED 0x6d2b79f5U
#define PCM_HASH_OFFSET 2166136261U
#define PCM_HASH_PRIME 16777619U
#define FILTER_TABLE_SIZE 256U
#define FILTER_CONTROL_FRAMES 8U
#define FILTER_MIX_MAX 65535U
#define FILTER_MIX_STEP 2048U
#define FILTER_PI 3.14159265358979323846f
#define FILTER_STATE_LIMIT 8.0f

typedef enum {
    VOICE_STAGE_OFF = 0,
    VOICE_STAGE_ATTACK,
    VOICE_STAGE_DECAY,
    VOICE_STAGE_SUSTAIN,
    VOICE_STAGE_RELEASE,
} voice_stage_t;

typedef struct {
    bool active;
    uint32_t frequency_hz;
    uint8_t velocity;
    solar_os_synth_voice_config_t config;
    voice_stage_t stage;
    uint32_t envelope;
    uint32_t envelope_step;
    voice_stage_t filter_stage;
    uint32_t filter_envelope;
    uint32_t filter_envelope_step;
    uint32_t phase;
    uint32_t phase_step;
    uint32_t sample_rate;
    uint32_t noise;
    uint32_t age;
    float filter_ic1eq;
    float filter_ic2eq;
    float filter_a1;
    float filter_a2;
    float filter_a3;
    uint16_t filter_coefficient_index;
    uint8_t filter_coefficient_resonance;
    uint8_t filter_control_countdown;
    uint16_t filter_mix;
} synth_voice_t;

typedef struct {
    SemaphoreHandle_t mutex;
    bool claimed;
    char owner[SOLAR_OS_SYNTH_OWNER_MAX];
    solar_os_synth_voice_config_t config;
    synth_voice_t voices[SOLAR_OS_SYNTH_VOICE_MAX];
    uint32_t next_age;
    uint32_t stolen_voices;
    solar_os_synth_waveform_t pcm_waveform;
    uint32_t pcm_generation;
    uint32_t pcm_hash;
    uint32_t pcm_mean_abs;
    int16_t pcm_min;
    int16_t pcm_max;
    size_t pcm_sample_count;
    int16_t pcm_samples[SOLAR_OS_SYNTH_VOICE_SCOPE_SAMPLES];
    int16_t wavetable[SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES];
    uint32_t filter_table_sample_rate;
    float filter_g[FILTER_TABLE_SIZE];
} voice_state_t;

static voice_state_t voice_state;
static StaticSemaphore_t voice_mutex_storage;
static portMUX_TYPE voice_init_lock = portMUX_INITIALIZER_UNLOCKED;

static const solar_os_synth_voice_config_t default_config = {
    .waveform = SOLAR_OS_SYNTH_WAVE_SQUARE,
    .attack_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_ATTACK_MS,
    .decay_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_DECAY_MS,
    .sustain_percent = SOLAR_OS_SYNTH_VOICE_DEFAULT_SUSTAIN_PERCENT,
    .release_ms = SOLAR_OS_SYNTH_VOICE_DEFAULT_RELEASE_MS,
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

static const int16_t sine_quarter_wave[65] = {
    0,     804,   1608,  2410,  3212,  4011,  4808,  5602,  6393,  7179,
    7962,  8739,  9512,  10278, 11039, 11793, 12539, 13279, 14010, 14732,
    15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403,
    22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571,
    30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521,
    32609, 32678, 32728, 32757, 32767,
};

static void voice_init_wavetable(void)
{
    for (size_t i = 0; i < SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES; i++) {
        voice_state.wavetable[i] =
            i < SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES / 2U ? -32767 : 32767;
    }
}

static esp_err_t voice_ensure_mutex(void)
{
    portENTER_CRITICAL(&voice_init_lock);
    if (voice_state.mutex == NULL) {
        voice_state.mutex = xSemaphoreCreateMutexStatic(&voice_mutex_storage);
        voice_state.config = default_config;
        voice_init_wavetable();
    }
    SemaphoreHandle_t mutex = voice_state.mutex;
    portEXIT_CRITICAL(&voice_init_lock);
    return mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void voice_lock(void)
{
    (void)xSemaphoreTake(voice_state.mutex, portMAX_DELAY);
}

static void voice_unlock(void)
{
    (void)xSemaphoreGive(voice_state.mutex);
}

static bool voice_owner_matches(const char *owner)
{
    return owner != NULL && owner[0] != '\0' &&
           strcmp(owner, voice_state.owner) == 0;
}

static bool voice_config_valid(const solar_os_synth_voice_config_t *config)
{
    return config != NULL &&
           config->waveform >= SOLAR_OS_SYNTH_WAVE_SQUARE &&
           config->waveform <= SOLAR_OS_SYNTH_WAVE_CUSTOM &&
           config->attack_ms <= SOLAR_OS_SYNTH_VOICE_ENVELOPE_MAX_MS &&
           config->decay_ms <= SOLAR_OS_SYNTH_VOICE_ENVELOPE_MAX_MS &&
           config->sustain_percent <= 100U &&
           config->release_ms <= SOLAR_OS_SYNTH_VOICE_ENVELOPE_MAX_MS &&
           config->filter.cutoff_hz >=
               SOLAR_OS_SYNTH_VOICE_FILTER_CUTOFF_MIN_HZ &&
           config->filter.cutoff_hz <=
               SOLAR_OS_SYNTH_VOICE_FILTER_CUTOFF_MAX_HZ &&
           config->filter.resonance_percent <= 100U &&
           config->filter.envelope_amount_percent <= 100U &&
           config->filter.attack_ms <=
               SOLAR_OS_SYNTH_VOICE_ENVELOPE_MAX_MS &&
           config->filter.decay_ms <=
               SOLAR_OS_SYNTH_VOICE_ENVELOPE_MAX_MS &&
           config->filter.sustain_percent <= 100U &&
           config->filter.release_ms <=
               SOLAR_OS_SYNTH_VOICE_ENVELOPE_MAX_MS;
}

static uint32_t envelope_step(uint32_t distance,
                              uint32_t duration_ms,
                              uint32_t sample_rate)
{
    if (distance == 0) {
        return 0;
    }
    if (duration_ms == 0 || sample_rate == 0) {
        return distance;
    }
    const uint64_t frames = ((uint64_t)sample_rate * duration_ms) / 1000U;
    if (frames == 0) {
        return distance;
    }
    const uint64_t step = ((uint64_t)distance + frames - 1U) / frames;
    return step > UINT32_MAX ? UINT32_MAX : (uint32_t)step;
}

static uint32_t sustain_level(const synth_voice_t *voice)
{
    return ((uint32_t)voice->config.sustain_percent * ENVELOPE_MAX) / 100U;
}

static uint32_t filter_sustain_level(const synth_voice_t *voice)
{
    return ((uint32_t)voice->config.filter.sustain_percent * ENVELOPE_MAX) /
           100U;
}

static uint32_t filter_frequency_for_index(size_t index,
                                           uint32_t sample_rate)
{
    uint32_t maximum = SOLAR_OS_SYNTH_VOICE_FILTER_CUTOFF_MAX_HZ;
    if (sample_rate > 400U && maximum >= sample_rate / 2U) {
        maximum = sample_rate / 2U - 200U;
    }
    if (maximum < SOLAR_OS_SYNTH_VOICE_FILTER_CUTOFF_MIN_HZ) {
        maximum = SOLAR_OS_SYNTH_VOICE_FILTER_CUTOFF_MIN_HZ;
    }
    const uint64_t position = (uint64_t)index * index;
    const uint64_t denominator =
        (uint64_t)(FILTER_TABLE_SIZE - 1U) * (FILTER_TABLE_SIZE - 1U);
    return SOLAR_OS_SYNTH_VOICE_FILTER_CUTOFF_MIN_HZ +
           (uint32_t)(((uint64_t)(maximum -
                                 SOLAR_OS_SYNTH_VOICE_FILTER_CUTOFF_MIN_HZ) *
                       position) /
                      denominator);
}

static void voice_prepare_filter_table(uint32_t sample_rate)
{
    if (sample_rate == 0 || voice_state.filter_table_sample_rate == sample_rate) {
        return;
    }
    for (size_t i = 0; i < FILTER_TABLE_SIZE; i++) {
        const float frequency =
            (float)filter_frequency_for_index(i, sample_rate);
        voice_state.filter_g[i] =
            tanf(FILTER_PI * frequency / (float)sample_rate);
    }
    voice_state.filter_table_sample_rate = sample_rate;
    for (size_t i = 0; i < SOLAR_OS_SYNTH_VOICE_MAX; i++) {
        voice_state.voices[i].filter_coefficient_index = UINT16_MAX;
        voice_state.voices[i].filter_control_countdown = 0;
    }
}

static uint16_t voice_filter_index_for_frequency(uint32_t frequency_hz,
                                                 uint32_t sample_rate)
{
    size_t low = 0;
    size_t high = FILTER_TABLE_SIZE - 1U;
    while (low < high) {
        const size_t middle = (low + high) / 2U;
        if (filter_frequency_for_index(middle, sample_rate) < frequency_hz) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    if (low > 0U) {
        const uint32_t upper = filter_frequency_for_index(low, sample_rate);
        const uint32_t lower = filter_frequency_for_index(low - 1U, sample_rate);
        const uint32_t lower_distance =
            frequency_hz > lower ? frequency_hz - lower : lower - frequency_hz;
        const uint32_t upper_distance =
            frequency_hz > upper ? frequency_hz - upper : upper - frequency_hz;
        if (lower_distance < upper_distance) {
            low--;
        }
    }
    return (uint16_t)low;
}

static void voice_enter_stage(synth_voice_t *voice,
                              voice_stage_t stage,
                              uint32_t sample_rate)
{
    voice->stage = stage;
    switch (stage) {
    case VOICE_STAGE_ATTACK:
        voice->envelope_step = envelope_step(ENVELOPE_MAX - voice->envelope,
                                             voice->config.attack_ms,
                                             sample_rate);
        break;
    case VOICE_STAGE_DECAY: {
        const uint32_t sustain = sustain_level(voice);
        voice->envelope_step = envelope_step(voice->envelope > sustain
                                                 ? voice->envelope - sustain
                                                 : 0,
                                             voice->config.decay_ms,
                                             sample_rate);
        break;
    }
    case VOICE_STAGE_RELEASE:
        voice->envelope_step = envelope_step(voice->envelope,
                                             voice->config.release_ms,
                                             sample_rate);
        break;
    default:
        voice->envelope_step = 0;
        break;
    }
}

static void voice_enter_filter_stage(synth_voice_t *voice,
                                     voice_stage_t stage,
                                     uint32_t sample_rate)
{
    voice->filter_stage = stage;
    switch (stage) {
    case VOICE_STAGE_ATTACK:
        voice->filter_envelope_step = envelope_step(
            ENVELOPE_MAX - voice->filter_envelope,
            voice->config.filter.attack_ms,
            sample_rate);
        break;
    case VOICE_STAGE_DECAY: {
        const uint32_t sustain = filter_sustain_level(voice);
        voice->filter_envelope_step = envelope_step(
            voice->filter_envelope > sustain
                ? voice->filter_envelope - sustain
                : 0,
            voice->config.filter.decay_ms,
            sample_rate);
        break;
    }
    case VOICE_STAGE_RELEASE:
        voice->filter_envelope_step = envelope_step(
            voice->filter_envelope,
            voice->config.filter.release_ms,
            sample_rate);
        break;
    default:
        voice->filter_envelope_step = 0;
        break;
    }
}

static void voice_prepare_rate(synth_voice_t *voice, uint32_t sample_rate)
{
    if (voice->sample_rate == sample_rate) {
        return;
    }
    voice->sample_rate = sample_rate;
    voice->phase_step = sample_rate > 0
                            ? (uint32_t)(((uint64_t)voice->frequency_hz << 32) /
                                         sample_rate)
                            : 0;
    voice_enter_stage(voice, voice->stage, sample_rate);
    voice_enter_filter_stage(voice, voice->filter_stage, sample_rate);
    voice->filter_coefficient_index = UINT16_MAX;
    voice->filter_control_countdown = 0;
}

static void voice_advance_envelope(synth_voice_t *voice, uint32_t sample_rate)
{
    switch (voice->stage) {
    case VOICE_STAGE_ATTACK:
        if (voice->envelope_step >= ENVELOPE_MAX - voice->envelope) {
            voice->envelope = ENVELOPE_MAX;
            voice_enter_stage(voice, VOICE_STAGE_DECAY, sample_rate);
        } else {
            voice->envelope += voice->envelope_step;
        }
        break;
    case VOICE_STAGE_DECAY: {
        const uint32_t sustain = sustain_level(voice);
        if (voice->envelope <= sustain ||
            voice->envelope_step >= voice->envelope - sustain) {
            voice->envelope = sustain;
            voice_enter_stage(voice, VOICE_STAGE_SUSTAIN, sample_rate);
        } else {
            voice->envelope -= voice->envelope_step;
        }
        break;
    }
    case VOICE_STAGE_RELEASE:
        if (voice->envelope_step >= voice->envelope) {
            memset(voice, 0, sizeof(*voice));
        } else {
            voice->envelope -= voice->envelope_step;
        }
        break;
    default:
        break;
    }
}

static void voice_advance_filter_envelope(synth_voice_t *voice,
                                          uint32_t sample_rate)
{
    switch (voice->filter_stage) {
    case VOICE_STAGE_ATTACK:
        if (voice->filter_envelope_step >=
            ENVELOPE_MAX - voice->filter_envelope) {
            voice->filter_envelope = ENVELOPE_MAX;
            voice_enter_filter_stage(voice, VOICE_STAGE_DECAY, sample_rate);
        } else {
            voice->filter_envelope += voice->filter_envelope_step;
        }
        break;
    case VOICE_STAGE_DECAY: {
        const uint32_t sustain = filter_sustain_level(voice);
        if (voice->filter_envelope <= sustain ||
            voice->filter_envelope_step >=
                voice->filter_envelope - sustain) {
            voice->filter_envelope = sustain;
            voice_enter_filter_stage(voice, VOICE_STAGE_SUSTAIN, sample_rate);
        } else {
            voice->filter_envelope -= voice->filter_envelope_step;
        }
        break;
    }
    case VOICE_STAGE_RELEASE:
        if (voice->filter_envelope_step >= voice->filter_envelope) {
            voice->filter_envelope = 0;
            voice_enter_filter_stage(voice, VOICE_STAGE_OFF, sample_rate);
        } else {
            voice->filter_envelope -= voice->filter_envelope_step;
        }
        break;
    default:
        break;
    }
}

static int32_t voice_periodic_sample(solar_os_synth_waveform_t waveform,
                                     uint32_t phase)
{
    switch (waveform) {
    case SOLAR_OS_SYNTH_WAVE_CUSTOM: {
        const size_t index = phase >> 24;
        const size_t next =
            (index + 1U) & (SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES - 1U);
        const uint32_t fraction = (phase >> 8) & 0xffffU;
        const int32_t first = voice_state.wavetable[index];
        const int32_t difference = voice_state.wavetable[next] - first;
        return first +
               (int32_t)(((int64_t)difference * fraction) >> 16);
    }
    case SOLAR_OS_SYNTH_WAVE_TRIANGLE:
        return phase < 0x80000000U
                   ? -32767 + (int32_t)(phase >> 15)
                   : 32767 - (int32_t)((phase - 0x80000000U) >> 15);
    case SOLAR_OS_SYNTH_WAVE_SAW:
        return (int32_t)(phase >> 16) - 32768;
    case SOLAR_OS_SYNTH_WAVE_SINE: {
        const uint32_t quadrant = phase >> 30;
        const size_t offset = (phase >> 24) & 0x3fU;
        const uint32_t fraction = (phase >> 8) & 0xffffU;
        size_t first_index = offset;
        size_t second_index = offset + 1U;
        if ((quadrant & 1U) != 0U) {
            first_index = 64U - offset;
            second_index = first_index - 1U;
        }
        const int32_t first = sine_quarter_wave[first_index];
        const int32_t difference = sine_quarter_wave[second_index] - first;
        int32_t sample =
            first + (int32_t)(((int64_t)difference * fraction) >> 16);
        if (quadrant >= 2U) {
            sample = -sample;
        }
        return sample;
    }
    case SOLAR_OS_SYNTH_WAVE_SQUARE:
    default:
        return (phase & 0x80000000U) != 0 ? 32767 : -32767;
    }
}

static int32_t voice_wave_sample(synth_voice_t *voice)
{
    const uint32_t previous = voice->phase;
    voice->phase += voice->phase_step;

    if (voice->config.waveform == SOLAR_OS_SYNTH_WAVE_NOISE) {
        if (voice->phase < previous) {
            uint32_t noise = voice->noise != 0 ? voice->noise : NOISE_SEED;
            noise ^= noise << 13;
            noise ^= noise >> 17;
            noise ^= noise << 5;
            voice->noise = noise;
        }
        return (voice->noise & 1U) != 0 ? 32767 : -32767;
    }

    int64_t accumulated = 0;
    for (uint32_t i = 0; i < VOICE_OVERSAMPLE; i++) {
        const uint64_t numerator =
            (uint64_t)voice->phase_step * ((i * 2U) + 1U);
        const uint32_t phase = previous +
            (uint32_t)(numerator / (VOICE_OVERSAMPLE * 2U));
        accumulated += voice_periodic_sample(voice->config.waveform, phase);
    }
    return (int32_t)(accumulated / (int32_t)VOICE_OVERSAMPLE);
}

static uint32_t voice_filter_cutoff(const synth_voice_t *voice)
{
    const uint32_t base = voice->config.filter.cutoff_hz;
    const uint32_t remaining =
        SOLAR_OS_SYNTH_VOICE_FILTER_CUTOFF_MAX_HZ - base;
    const uint64_t modulation =
        (uint64_t)remaining *
        voice->config.filter.envelope_amount_percent *
        voice->filter_envelope;
    return base + (uint32_t)(modulation / (100U * ENVELOPE_MAX));
}

static bool voice_filter_enabled(const synth_voice_t *voice)
{
    return voice->config.filter.cutoff_hz <
               SOLAR_OS_SYNTH_VOICE_FILTER_CUTOFF_MAX_HZ ||
           voice->config.filter.resonance_percent > 0U;
}

static float voice_filter_soft_clip(float sample)
{
    const float magnitude = sample < 0.0f ? -sample : sample;
    if (magnitude <= 0.8f) {
        return sample;
    }
    if (magnitude >= 2.0f) {
        return sample < 0.0f ? -1.0f : 1.0f;
    }
    const float position = (magnitude - 0.8f) / 1.2f;
    const float clipped = 0.8f + 0.2f * (2.0f * position - position * position);
    return sample < 0.0f ? -clipped : clipped;
}

static void voice_update_filter_coefficients(synth_voice_t *voice,
                                             uint32_t sample_rate)
{
    const uint16_t index = voice_filter_index_for_frequency(
        voice_filter_cutoff(voice), sample_rate);
    const uint8_t resonance = voice->config.filter.resonance_percent;
    if (index == voice->filter_coefficient_index &&
        resonance == voice->filter_coefficient_resonance) {
        return;
    }

    const float normalized = (float)resonance / 100.0f;
    const float quality = 0.5f + 9.5f * normalized * normalized;
    const float damping = 1.0f / quality;
    const float g = voice_state.filter_g[index];
    voice->filter_a1 = 1.0f / (1.0f + g * (g + damping));
    voice->filter_a2 = g * voice->filter_a1;
    voice->filter_a3 = g * voice->filter_a2;
    voice->filter_coefficient_index = index;
    voice->filter_coefficient_resonance = resonance;
}

static int32_t voice_filter_sample(synth_voice_t *voice,
                                   int32_t dry,
                                   uint32_t sample_rate)
{
    const bool enabled = voice_filter_enabled(voice);
    if (!enabled && voice->filter_mix == 0U) {
        voice->filter_ic1eq = 0.0f;
        voice->filter_ic2eq = 0.0f;
        return dry;
    }

    if (voice->filter_control_countdown == 0U) {
        voice_update_filter_coefficients(voice, sample_rate);
        voice->filter_control_countdown = FILTER_CONTROL_FRAMES - 1U;
    } else {
        voice->filter_control_countdown--;
    }

    const float input = (float)dry / 32768.0f;
    const float v3 = input - voice->filter_ic2eq;
    const float v1 = voice->filter_a1 * voice->filter_ic1eq +
                     voice->filter_a2 * v3;
    const float v2 = voice->filter_ic2eq +
                     voice->filter_a2 * voice->filter_ic1eq +
                     voice->filter_a3 * v3;
    voice->filter_ic1eq = 2.0f * v1 - voice->filter_ic1eq;
    voice->filter_ic2eq = 2.0f * v2 - voice->filter_ic2eq;

    if (!isfinite(voice->filter_ic1eq) ||
        !isfinite(voice->filter_ic2eq)) {
        voice->filter_ic1eq = 0.0f;
        voice->filter_ic2eq = 0.0f;
    } else {
        if (voice->filter_ic1eq > FILTER_STATE_LIMIT) {
            voice->filter_ic1eq = FILTER_STATE_LIMIT;
        } else if (voice->filter_ic1eq < -FILTER_STATE_LIMIT) {
            voice->filter_ic1eq = -FILTER_STATE_LIMIT;
        }
        if (voice->filter_ic2eq > FILTER_STATE_LIMIT) {
            voice->filter_ic2eq = FILTER_STATE_LIMIT;
        } else if (voice->filter_ic2eq < -FILTER_STATE_LIMIT) {
            voice->filter_ic2eq = -FILTER_STATE_LIMIT;
        }
    }

    float filtered = v2;
    if (!isfinite(filtered)) {
        filtered = 0.0f;
    } else {
        filtered = voice_filter_soft_clip(filtered);
    }
    const int32_t wet = (int32_t)(filtered * 32767.0f);

    const uint16_t target_mix = enabled ? FILTER_MIX_MAX : 0U;
    if (voice->filter_mix < target_mix) {
        const uint32_t next = voice->filter_mix + FILTER_MIX_STEP;
        voice->filter_mix =
            next > target_mix ? target_mix : (uint16_t)next;
    } else if (voice->filter_mix > target_mix) {
        const uint32_t distance = voice->filter_mix - target_mix;
        voice->filter_mix = distance > FILTER_MIX_STEP
                                ? (uint16_t)(voice->filter_mix - FILTER_MIX_STEP)
                                : target_mix;
    }

    return (int32_t)(((int64_t)dry * (FILTER_MIX_MAX - voice->filter_mix) +
                      (int64_t)wet * voice->filter_mix) /
                     FILTER_MIX_MAX);
}

static void voice_render(int16_t *samples,
                         size_t frames,
                         uint32_t sample_rate,
                         void *user)
{
    (void)user;
    if (samples == NULL || sample_rate == 0 || voice_state.mutex == NULL ||
        xSemaphoreTake(voice_state.mutex, 0) != pdTRUE) {
        return;
    }

    int16_t pcm_samples[SOLAR_OS_SYNTH_VOICE_SCOPE_SAMPLES];
    size_t pcm_sample_count = 0;
    int16_t pcm_min = INT16_MAX;
    int16_t pcm_max = INT16_MIN;
    uint64_t pcm_abs_total = 0;
    size_t pcm_active_frames = 0;
    uint32_t pcm_hash = PCM_HASH_OFFSET;
    bool pcm_active = false;

    for (size_t frame = 0; frame < frames; frame++) {
        int32_t mixed = 0;
        uint32_t active_voices = 0;
        uint32_t envelope_total = 0;
        for (size_t i = 0; i < SOLAR_OS_SYNTH_VOICE_MAX; i++) {
            synth_voice_t *voice = &voice_state.voices[i];
            if (!voice->active) {
                continue;
            }
            voice_prepare_rate(voice, sample_rate);
            const int32_t wave = voice_filter_sample(
                voice, voice_wave_sample(voice), sample_rate);
            /* Keep this division signed. The public maximum is an unsigned
             * constant, which would otherwise convert negative PCM to a large
             * positive value before division and clip it to +INT16_MAX. */
            const int32_t velocity_sample =
                (wave * (int32_t)voice->velocity) /
                (int32_t)SOLAR_OS_SYNTH_VOICE_VELOCITY_MAX;
            mixed += (int32_t)(((int64_t)velocity_sample * voice->envelope) >> 16);
            active_voices++;
            envelope_total += voice->envelope;
            voice_advance_filter_envelope(voice, sample_rate);
            voice_advance_envelope(voice, sample_rate);
        }
        /* Counting active slots causes a hard gain step when a silent attack
         * slot appears or a faded release slot disappears. Normalize only the
         * envelope energy above one full voice so polyphonic headroom follows
         * attack and release continuously. */
        if (envelope_total > ENVELOPE_MAX) {
            mixed = (int32_t)(((int64_t)mixed * ENVELOPE_MAX) /
                              envelope_total);
        }
        /* Match the proven tone-generator headroom before codec volume. */
        mixed = (int32_t)(((int64_t)mixed * VOICE_OUTPUT_PEAK) / INT16_MAX);
        if (mixed > INT16_MAX) {
            mixed = INT16_MAX;
        } else if (mixed < INT16_MIN) {
            mixed = INT16_MIN;
        }
        const int16_t pcm = (int16_t)mixed;
        samples[frame * 2U] = pcm;
        samples[(frame * 2U) + 1U] = pcm;

        if (active_voices > 0U) {
            pcm_active = true;
            pcm_active_frames++;
            if (pcm < pcm_min) {
                pcm_min = pcm;
            }
            if (pcm > pcm_max) {
                pcm_max = pcm;
            }
            pcm_abs_total += pcm < 0 ? -(int32_t)pcm : pcm;
            pcm_hash ^= (uint16_t)pcm;
            pcm_hash *= PCM_HASH_PRIME;
            if (pcm_sample_count < SOLAR_OS_SYNTH_VOICE_SCOPE_SAMPLES) {
                pcm_samples[pcm_sample_count++] = pcm;
            }
        }
    }

    if (pcm_active &&
        pcm_sample_count == SOLAR_OS_SYNTH_VOICE_SCOPE_SAMPLES) {
        voice_state.pcm_waveform = voice_state.config.waveform;
        voice_state.pcm_generation++;
        voice_state.pcm_hash = pcm_hash;
        voice_state.pcm_mean_abs =
            (uint32_t)(pcm_abs_total / pcm_active_frames);
        voice_state.pcm_min = pcm_min;
        voice_state.pcm_max = pcm_max;
        voice_state.pcm_sample_count = pcm_sample_count;
        memcpy(voice_state.pcm_samples,
               pcm_samples,
               pcm_sample_count * sizeof(pcm_samples[0]));
    }

    voice_unlock();
}

static esp_err_t voice_claim(const char *owner)
{
    if (owner == NULL || owner[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = voice_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }

    voice_lock();
    if (voice_state.claimed && !voice_owner_matches(owner)) {
        voice_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (voice_state.claimed) {
        voice_unlock();
        solar_os_synth_status_t status;
        solar_os_synth_get_status(&status);
        if ((status.running || status.starting) &&
            strcmp(status.owner, owner) == 0) {
            return ESP_OK;
        }
        voice_lock();
        voice_state.claimed = false;
        voice_state.owner[0] = '\0';
        memset(voice_state.voices, 0, sizeof(voice_state.voices));
    }
    voice_state.claimed = true;
    strlcpy(voice_state.owner, owner, sizeof(voice_state.owner));
    voice_unlock();

    const solar_os_synth_config_t synth_config = {
        .owner = owner,
        .render = voice_render,
        .user = NULL,
        .block_frames = VOICE_BLOCK_FRAMES,
    };
    err = solar_os_synth_start(&synth_config);
    if (err != ESP_OK) {
        voice_lock();
        if (voice_owner_matches(owner)) {
            voice_state.claimed = false;
            voice_state.owner[0] = '\0';
            memset(voice_state.voices, 0, sizeof(voice_state.voices));
        }
        voice_unlock();
    } else {
        solar_os_synth_status_t status;
        solar_os_synth_get_status(&status);
        voice_lock();
        voice_prepare_filter_table(status.sample_rate);
        voice_unlock();
    }
    return err;
}

const char *solar_os_synth_waveform_name(solar_os_synth_waveform_t waveform)
{
    switch (waveform) {
    case SOLAR_OS_SYNTH_WAVE_SQUARE:
        return "square";
    case SOLAR_OS_SYNTH_WAVE_TRIANGLE:
        return "triangle";
    case SOLAR_OS_SYNTH_WAVE_SAW:
        return "saw";
    case SOLAR_OS_SYNTH_WAVE_SINE:
        return "sine";
    case SOLAR_OS_SYNTH_WAVE_NOISE:
        return "noise";
    case SOLAR_OS_SYNTH_WAVE_CUSTOM:
        return "custom";
    default:
        return "unknown";
    }
}

bool solar_os_synth_parse_waveform(const char *name,
                                   solar_os_synth_waveform_t *waveform)
{
    if (name == NULL || waveform == NULL) {
        return false;
    }
    for (int value = SOLAR_OS_SYNTH_WAVE_SQUARE;
         value <= SOLAR_OS_SYNTH_WAVE_NOISE;
         value++) {
        const solar_os_synth_waveform_t candidate =
            (solar_os_synth_waveform_t)value;
        if (strcmp(name, solar_os_synth_waveform_name(candidate)) == 0) {
            *waveform = candidate;
            return true;
        }
    }
    return false;
}

esp_err_t solar_os_synth_voice_configure(
    const char *owner,
    const solar_os_synth_voice_config_t *config)
{
    if (owner == NULL || owner[0] == '\0' || !voice_config_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = voice_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }
    voice_lock();
    if (voice_state.claimed && !voice_owner_matches(owner)) {
        voice_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    voice_state.config = *config;
    for (size_t i = 0; i < SOLAR_OS_SYNTH_VOICE_MAX; i++) {
        synth_voice_t *voice = &voice_state.voices[i];
        if (!voice->active) {
            continue;
        }
        voice->config = *config;
        if (voice->stage == VOICE_STAGE_SUSTAIN) {
            voice->envelope = sustain_level(voice);
        } else {
            voice_enter_stage(voice, voice->stage, voice->sample_rate);
        }
        if (voice->filter_stage == VOICE_STAGE_SUSTAIN) {
            voice->filter_envelope = filter_sustain_level(voice);
        } else {
            voice_enter_filter_stage(voice,
                                     voice->filter_stage,
                                     voice->sample_rate);
        }
        voice->filter_coefficient_index = UINT16_MAX;
        voice->filter_control_countdown = 0;
    }
    voice_unlock();
    return ESP_OK;
}

esp_err_t solar_os_synth_voice_note_on(const char *owner,
                                       uint32_t frequency_hz,
                                       uint8_t velocity)
{
    if (frequency_hz < SOLAR_OS_SYNTH_VOICE_FREQUENCY_MIN_HZ ||
        frequency_hz > SOLAR_OS_SYNTH_VOICE_FREQUENCY_MAX_HZ ||
        velocity == 0 || velocity > SOLAR_OS_SYNTH_VOICE_VELOCITY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = voice_claim(owner);
    if (err != ESP_OK) {
        return err;
    }

    voice_lock();
    synth_voice_t *selected = NULL;
    synth_voice_t *oldest = NULL;
    synth_voice_t *oldest_releasing = NULL;
    for (size_t i = 0; i < SOLAR_OS_SYNTH_VOICE_MAX; i++) {
        synth_voice_t *voice = &voice_state.voices[i];
        if (voice->active && voice->frequency_hz == frequency_hz) {
            selected = voice;
            break;
        }
        if (!voice->active && selected == NULL) {
            selected = voice;
        }
        if (voice->active && (oldest == NULL || voice->age < oldest->age)) {
            oldest = voice;
        }
        if (voice->active && voice->stage == VOICE_STAGE_RELEASE &&
            (oldest_releasing == NULL || voice->age < oldest_releasing->age)) {
            oldest_releasing = voice;
        }
    }
    if (selected == NULL) {
        selected = oldest_releasing != NULL ? oldest_releasing : oldest;
        voice_state.stolen_voices++;
    }
    memset(selected, 0, sizeof(*selected));
    selected->active = true;
    selected->frequency_hz = frequency_hz;
    selected->velocity = velocity;
    selected->config = voice_state.config;
    selected->noise = NOISE_SEED ^ frequency_hz;
    selected->age = ++voice_state.next_age;
    selected->filter_coefficient_index = UINT16_MAX;
    voice_enter_stage(selected, VOICE_STAGE_ATTACK, 0);
    voice_enter_filter_stage(selected, VOICE_STAGE_ATTACK, 0);
    voice_unlock();
    return ESP_OK;
}

esp_err_t solar_os_synth_voice_note_off(const char *owner,
                                        uint32_t frequency_hz)
{
    if (frequency_hz < SOLAR_OS_SYNTH_VOICE_FREQUENCY_MIN_HZ ||
        frequency_hz > SOLAR_OS_SYNTH_VOICE_FREQUENCY_MAX_HZ) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = voice_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }
    voice_lock();
    if (!voice_state.claimed || !voice_owner_matches(owner)) {
        const bool claimed = voice_state.claimed;
        voice_unlock();
        return claimed ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    for (size_t i = 0; i < SOLAR_OS_SYNTH_VOICE_MAX; i++) {
        synth_voice_t *voice = &voice_state.voices[i];
        if (voice->active && voice->frequency_hz == frequency_hz &&
            voice->stage != VOICE_STAGE_RELEASE) {
            voice_enter_stage(voice, VOICE_STAGE_RELEASE, voice->sample_rate);
            voice_enter_filter_stage(voice,
                                     VOICE_STAGE_RELEASE,
                                     voice->sample_rate);
            break;
        }
    }
    voice_unlock();
    return ESP_OK;
}

esp_err_t solar_os_synth_voice_all_notes_off(const char *owner)
{
    esp_err_t err = voice_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }
    voice_lock();
    if (!voice_state.claimed || !voice_owner_matches(owner)) {
        const bool claimed = voice_state.claimed;
        voice_unlock();
        return claimed ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    for (size_t i = 0; i < SOLAR_OS_SYNTH_VOICE_MAX; i++) {
        synth_voice_t *voice = &voice_state.voices[i];
        if (voice->active && voice->stage != VOICE_STAGE_RELEASE) {
            voice_enter_stage(voice, VOICE_STAGE_RELEASE, voice->sample_rate);
            voice_enter_filter_stage(voice,
                                     VOICE_STAGE_RELEASE,
                                     voice->sample_rate);
        }
    }
    voice_unlock();
    return ESP_OK;
}

esp_err_t solar_os_synth_voice_stop(const char *owner)
{
    esp_err_t err = voice_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }
    voice_lock();
    if (!voice_state.claimed) {
        voice_unlock();
        return ESP_OK;
    }
    if (!voice_owner_matches(owner)) {
        voice_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memset(voice_state.voices, 0, sizeof(voice_state.voices));
    voice_unlock();

    err = solar_os_synth_stop(owner);
    if (err == ESP_OK) {
        voice_lock();
        if (voice_owner_matches(owner)) {
            voice_state.claimed = false;
            voice_state.owner[0] = '\0';
        }
        voice_unlock();
    }
    return err;
}

esp_err_t solar_os_synth_voice_set_wavetable(const char *owner,
                                             const int16_t *samples,
                                             size_t sample_count)
{
    if (owner == NULL || owner[0] == '\0' || samples == NULL ||
        sample_count != SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = voice_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }
    voice_lock();
    if (voice_state.claimed && !voice_owner_matches(owner)) {
        voice_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(voice_state.wavetable,
           samples,
           sizeof(voice_state.wavetable));
    voice_unlock();
    return ESP_OK;
}

esp_err_t solar_os_synth_voice_get_wavetable(int16_t *samples,
                                             size_t sample_count)
{
    if (samples == NULL ||
        sample_count != SOLAR_OS_SYNTH_VOICE_WAVETABLE_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = voice_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }
    voice_lock();
    memcpy(samples, voice_state.wavetable, sizeof(voice_state.wavetable));
    voice_unlock();
    return ESP_OK;
}

void solar_os_synth_voice_get_status(solar_os_synth_voice_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (voice_ensure_mutex() != ESP_OK) {
        status->last_error = ESP_ERR_NO_MEM;
        return;
    }

    voice_lock();
    strlcpy(status->owner, voice_state.owner, sizeof(status->owner));
    status->config = voice_state.config;
    status->stolen_voices = voice_state.stolen_voices;
    status->pcm_waveform = voice_state.pcm_waveform;
    status->pcm_generation = voice_state.pcm_generation;
    status->pcm_hash = voice_state.pcm_hash;
    status->pcm_mean_abs = voice_state.pcm_mean_abs;
    status->pcm_min = voice_state.pcm_min;
    status->pcm_max = voice_state.pcm_max;
    status->pcm_sample_count = voice_state.pcm_sample_count;
    memcpy(status->pcm_samples,
           voice_state.pcm_samples,
           voice_state.pcm_sample_count * sizeof(status->pcm_samples[0]));
    for (size_t i = 0; i < SOLAR_OS_SYNTH_VOICE_MAX; i++) {
        if (voice_state.voices[i].active) {
            status->active_voices++;
        }
    }
    voice_unlock();

    solar_os_synth_status_t synth_status;
    solar_os_synth_get_status(&synth_status);
    status->running = synth_status.running && status->owner[0] != '\0' &&
                      strcmp(synth_status.owner, status->owner) == 0;
    status->sample_rate = synth_status.sample_rate;
    status->render_deadline_misses = synth_status.render_deadline_misses;
    status->last_error = synth_status.last_error;
}
