#include "solar_os_synth_voice.h"

#include <limits.h>
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
    uint32_t phase;
    uint32_t phase_step;
    uint32_t sample_rate;
    uint32_t noise;
    uint32_t age;
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
};

static esp_err_t voice_ensure_mutex(void)
{
    portENTER_CRITICAL(&voice_init_lock);
    if (voice_state.mutex == NULL) {
        voice_state.mutex = xSemaphoreCreateMutexStatic(&voice_mutex_storage);
        voice_state.config = default_config;
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
           config->waveform <= SOLAR_OS_SYNTH_WAVE_NOISE &&
           config->attack_ms <= SOLAR_OS_SYNTH_VOICE_ENVELOPE_MAX_MS &&
           config->decay_ms <= SOLAR_OS_SYNTH_VOICE_ENVELOPE_MAX_MS &&
           config->sustain_percent <= 100U &&
           config->release_ms <= SOLAR_OS_SYNTH_VOICE_ENVELOPE_MAX_MS;
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

static int32_t voice_periodic_sample(solar_os_synth_waveform_t waveform,
                                     uint32_t phase)
{
    switch (waveform) {
    case SOLAR_OS_SYNTH_WAVE_TRIANGLE:
        return phase < 0x80000000U
                   ? -32767 + (int32_t)(phase >> 15)
                   : 32767 - (int32_t)((phase - 0x80000000U) >> 15);
    case SOLAR_OS_SYNTH_WAVE_SAW:
        return (int32_t)(phase >> 16) - 32768;
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
        uint32_t mixed_voices = 0;
        for (size_t i = 0; i < SOLAR_OS_SYNTH_VOICE_MAX; i++) {
            synth_voice_t *voice = &voice_state.voices[i];
            if (!voice->active) {
                continue;
            }
            voice_prepare_rate(voice, sample_rate);
            const int32_t wave = voice_wave_sample(voice);
            /* Keep this division signed. The public maximum is an unsigned
             * constant, which would otherwise convert negative PCM to a large
             * positive value before division and clip it to +INT16_MAX. */
            const int32_t velocity_sample =
                (wave * (int32_t)voice->velocity) /
                (int32_t)SOLAR_OS_SYNTH_VOICE_VELOCITY_MAX;
            mixed += (int32_t)(((int64_t)velocity_sample * voice->envelope) >> 16);
            mixed_voices++;
            voice_advance_envelope(voice, sample_rate);
        }
        if (mixed_voices > 1U) {
            mixed /= (int32_t)mixed_voices;
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

        if (mixed_voices > 0U) {
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
    case SOLAR_OS_SYNTH_WAVE_NOISE:
        return "noise";
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
    voice_enter_stage(selected, VOICE_STAGE_ATTACK, 0);
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
