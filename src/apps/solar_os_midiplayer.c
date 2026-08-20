#include "solar_os_midiplayer.h"

#include <ctype.h>
#include <dirent.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "solar_os.h"
#include "solar_os_appbar.h"
#include "solar_os_audio.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_resource_limits.h"
#include "solar_os_storage.h"
#include "solar_os_synth.h"
#include "solar_os_synth_voice.h"
#include "solar_os_task.h"
#include "solar_os_time.h"

#define TAG "midiplayer"
#define MIDIPLAYER_OWNER "midiplayer"
#define MIDIPLAYER_MAX_TRACKS 16
#define MIDIPLAYER_MAX_ACTIVE_NOTES 16
#define MIDIPLAYER_MAX_PLAYLIST 48
#define MIDIPLAYER_REFRESH_MS 80U

typedef enum {
    MIDIPLAYER_VIEW_PLAYER = 0,
    MIDIPLAYER_VIEW_PICKER = 1,
} midiplayer_view_t;

typedef struct {
    const uint8_t *start;
    const uint8_t *ptr;
    const uint8_t *end;
    uint32_t next_event_tick;
    uint8_t running_status;
    bool end_of_track;
} midi_track_t;

typedef struct {
    uint8_t note;
    uint8_t channel;
    uint32_t frequency_hz;
    bool active;
} active_note_t;

typedef struct {
    char path[128];
    char name[48];
    bool is_builtin;
} playlist_item_t;

typedef struct {
    solar_os_context_t *ctx;
    midiplayer_view_t view;

    /* Playback file / data */
    char current_path[128];
    char song_title[64];
    uint8_t *file_buffer;
    size_t file_size;
    bool is_builtin_demo;

    /* MIDI Header */
    uint16_t format;
    uint16_t track_count;
    uint16_t division; /* Ticks per quarter note (TPQN) */

    /* Tracks */
    midi_track_t tracks[MIDIPLAYER_MAX_TRACKS];

    /* Sequencer timing */
    uint32_t tempo_us_per_qn; /* default 500000 (120 BPM) */
    uint32_t current_bpm;
    uint32_t current_tick;
    uint32_t total_ticks;
    uint32_t elapsed_ms;
    uint32_t total_ms;

    /* Playback State */
    bool playing;
    bool paused;
    bool loop;
    uint32_t last_step_ms;
    uint32_t last_render_ms;

    /* Interactive Piano Click Auto-Release */
    uint8_t click_note;
    uint32_t click_release_at_ms;

    /* Active Notes & Visuals */
    active_note_t active_notes[MIDIPLAYER_MAX_ACTIVE_NOTES];
    uint8_t channel_activity[16]; /* 0..127 velocity / active state */
    uint8_t volume; /* 0..100 */
    solar_os_synth_waveform_t waveform;

    /* File Picker / Playlist */
    playlist_item_t playlist[MIDIPLAYER_MAX_PLAYLIST];
    size_t playlist_count;
    size_t selected_file_idx;
    size_t picker_scroll_offset;

    /* Help Modal */
    bool show_help;
} midiplayer_state_t;

static void *midiplayer_state_ptr;
#define mstate (*(midiplayer_state_t *)midiplayer_state_ptr)

/* ---------------- 100% Valid Embedded Demo SMF Type 0 (SolarOS Symphony) ---------------- */
static const uint8_t builtin_demo_midi[] = {
    0x4D, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x60,
    0x4D, 0x54, 0x72, 0x6B, 0x00, 0x00, 0x00, 0xBF,
    /* Track Name: SolarOS Symphony */
    0x00, 0xFF, 0x03, 0x10, 0x53, 0x6F, 0x6C, 0x61, 0x72, 0x4F, 0x53, 0x20, 0x53, 0x79, 0x6D, 0x70, 0x68, 0x6F, 0x6E, 0x79,
    /* Set Tempo: 500000 us (120 BPM) */
    0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,
    /* Pattern */
    0x00, 0x90, 0x3C, 0x64, 0x00, 0x91, 0x30, 0x50, 0x30, 0x80, 0x3C, 0x00,
    0x00, 0x90, 0x37, 0x5A, 0x30, 0x80, 0x37, 0x00, 0x00, 0x90, 0x3C, 0x64,
    0x30, 0x80, 0x3C, 0x00, 0x00, 0x90, 0x37, 0x5A, 0x30, 0x80, 0x37, 0x00,
    0x00, 0x90, 0x3C, 0x64, 0x18, 0x80, 0x3C, 0x00, 0x00, 0x90, 0x40, 0x64,
    0x18, 0x80, 0x40, 0x00, 0x00, 0x90, 0x43, 0x64, 0x30, 0x81, 0x30, 0x00,
    0x30, 0x80, 0x43, 0x00, 0x00, 0x90, 0x41, 0x64, 0x00, 0x91, 0x35, 0x50,
    0x30, 0x80, 0x41, 0x00, 0x00, 0x90, 0x3E, 0x5A, 0x30, 0x80, 0x3E, 0x00,
    0x00, 0x90, 0x41, 0x64, 0x30, 0x80, 0x41, 0x00, 0x00, 0x90, 0x3E, 0x5A,
    0x30, 0x80, 0x3E, 0x00, 0x00, 0x90, 0x3E, 0x64, 0x18, 0x80, 0x3E, 0x00,
    0x00, 0x90, 0x3B, 0x64, 0x18, 0x80, 0x3B, 0x00, 0x00, 0x90, 0x37, 0x64,
    0x30, 0x81, 0x35, 0x00, 0x30, 0x80, 0x37, 0x00, 0x00, 0x90, 0x3C, 0x64,
    0x00, 0x90, 0x40, 0x5A, 0x00, 0x90, 0x43, 0x5A, 0x00, 0x91, 0x24, 0x64,
    0x60, 0x80, 0x3C, 0x00, 0x00, 0x80, 0x40, 0x00, 0x00, 0x80, 0x43, 0x00,
    0x00, 0x81, 0x24, 0x00, 0x00, 0xFF, 0x2F, 0x00
};

static uint32_t midi_note_to_freq(uint8_t note)
{
    if (note < 12 || note > 127) return 0;
    const float f = 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
    if (f < (float)SOLAR_OS_SYNTH_VOICE_FREQUENCY_MIN_HZ ||
        f > (float)SOLAR_OS_SYNTH_VOICE_FREQUENCY_MAX_HZ) {
        return 0;
    }
    return (uint32_t)(f + 0.5f);
}

static uint32_t read_vlq(const uint8_t **ptr, const uint8_t *end)
{
    uint32_t value = 0;
    if (ptr == NULL || *ptr == NULL) return 0;
    while (*ptr < end) {
        const uint8_t b = **ptr;
        (*ptr)++;
        value = (value << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return value;
}

static void all_notes_off(void)
{
    solar_os_synth_voice_all_notes_off(MIDIPLAYER_OWNER);
    for (size_t i = 0; i < MIDIPLAYER_MAX_ACTIVE_NOTES; i++) {
        mstate.active_notes[i].active = false;
    }
    for (size_t i = 0; i < 16; i++) {
        mstate.channel_activity[i] = 0;
    }
}

static void note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    if (velocity == 0) {
        const uint32_t freq = midi_note_to_freq(note);
        if (freq > 0) {
            solar_os_synth_voice_note_off(MIDIPLAYER_OWNER, freq);
        }
        for (size_t i = 0; i < MIDIPLAYER_MAX_ACTIVE_NOTES; i++) {
            if (mstate.active_notes[i].active &&
                mstate.active_notes[i].note == note &&
                mstate.active_notes[i].channel == channel) {
                mstate.active_notes[i].active = false;
            }
        }
        return;
    }

    const uint32_t freq = midi_note_to_freq(note);
    if (freq == 0) return;

    const uint32_t scaled_vel = ((uint32_t)velocity * (uint32_t)mstate.volume) / 100U;
    solar_os_synth_voice_note_on(MIDIPLAYER_OWNER, freq, (uint8_t)(scaled_vel > 0 ? scaled_vel : 1));

    int slot = -1;
    for (size_t i = 0; i < MIDIPLAYER_MAX_ACTIVE_NOTES; i++) {
        if (!mstate.active_notes[i].active) {
            slot = (int)i;
            break;
        }
    }
    if (slot >= 0) {
        mstate.active_notes[slot].active = true;
        mstate.active_notes[slot].note = note;
        mstate.active_notes[slot].channel = channel;
        mstate.active_notes[slot].frequency_hz = freq;
    }

    if (channel < 16) {
        mstate.channel_activity[channel] = velocity;
    }
}

static void note_off(uint8_t channel, uint8_t note)
{
    const uint32_t freq = midi_note_to_freq(note);
    if (freq > 0) {
        solar_os_synth_voice_note_off(MIDIPLAYER_OWNER, freq);
    }
    for (size_t i = 0; i < MIDIPLAYER_MAX_ACTIVE_NOTES; i++) {
        if (mstate.active_notes[i].active &&
            mstate.active_notes[i].note == note &&
            mstate.active_notes[i].channel == channel) {
            mstate.active_notes[i].active = false;
        }
    }
}

static void reset_track(midi_track_t *tr)
{
    if (tr == NULL || tr->start == NULL) return;
    tr->ptr = tr->start;
    tr->running_status = 0;
    tr->end_of_track = false;
    if (tr->ptr < tr->end) {
        tr->next_event_tick = read_vlq(&tr->ptr, tr->end);
    } else {
        tr->end_of_track = true;
    }
}

static bool parse_midi_header(const uint8_t *data, size_t len)
{
    if (data == NULL || len < 14) return false;
    if (memcmp(data, "MThd", 4) != 0) return false;

    const uint32_t header_len = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                                ((uint32_t)data[6] << 8)  | (uint32_t)data[7];
    if (header_len < 6 || len < 8 + header_len) return false;

    mstate.format = ((uint16_t)data[8] << 8) | (uint16_t)data[9];
    mstate.track_count = ((uint16_t)data[10] << 8) | (uint16_t)data[11];
    mstate.division = ((uint16_t)data[12] << 8) | (uint16_t)data[13];

    if (mstate.division == 0) mstate.division = 96;
    if (mstate.track_count > MIDIPLAYER_MAX_TRACKS) {
        mstate.track_count = MIDIPLAYER_MAX_TRACKS;
    }

    size_t offset = 8 + header_len;
    size_t loaded_tracks = 0;
    uint32_t max_track_ticks = 0;

    for (size_t t = 0; t < mstate.track_count && offset + 8 <= len; t++) {
        if (memcmp(data + offset, "MTrk", 4) != 0) {
            offset++;
            continue;
        }
        const uint32_t track_len = ((uint32_t)data[offset + 4] << 24) | ((uint32_t)data[offset + 5] << 16) |
                                   ((uint32_t)data[offset + 6] << 8)  | (uint32_t)data[offset + 7];
        offset += 8;
        if (offset + track_len > len) break;

        mstate.tracks[loaded_tracks].start = data + offset;
        mstate.tracks[loaded_tracks].end = data + offset + track_len;
        reset_track(&mstate.tracks[loaded_tracks]);

        /* Scan track total ticks and track title */
        const uint8_t *scan_ptr = data + offset;
        const uint8_t *scan_end = data + offset + track_len;
        uint32_t track_ticks = 0;
        uint8_t scan_running = 0;

        while (scan_ptr < scan_end) {
            track_ticks += read_vlq(&scan_ptr, scan_end);
            if (scan_ptr >= scan_end) break;
            uint8_t s = *scan_ptr;
            if (s >= 0x80) {
                scan_ptr++;
                scan_running = s;
            } else {
                s = scan_running;
            }
            if (s == 0xFF) {
                if (scan_ptr >= scan_end) break;
                const uint8_t meta_type = *scan_ptr++;
                const uint32_t meta_len = read_vlq(&scan_ptr, scan_end);
                if (meta_type == 0x03 && mstate.song_title[0] == '\0' && meta_len > 0) {
                    const size_t cplen = meta_len < sizeof(mstate.song_title) - 1 ? meta_len : sizeof(mstate.song_title) - 1;
                    memcpy(mstate.song_title, scan_ptr, cplen);
                    mstate.song_title[cplen] = '\0';
                }
                scan_ptr += meta_len;
                if (meta_type == 0x2F) break;
            } else if (s == 0xF0 || s == 0xF7) {
                const uint32_t sysex_len = read_vlq(&scan_ptr, scan_end);
                scan_ptr += sysex_len;
            } else {
                const uint8_t cmd = s & 0xF0;
                if (cmd == 0xC0 || cmd == 0xD0) {
                    scan_ptr += 1;
                } else {
                    scan_ptr += 2;
                }
            }
        }
        if (track_ticks > max_track_ticks) {
            max_track_ticks = track_ticks;
        }

        offset += track_len;
        loaded_tracks++;
    }

    mstate.track_count = (uint16_t)loaded_tracks;
    mstate.total_ticks = max_track_ticks;
    mstate.current_tick = 0;
    mstate.tempo_us_per_qn = 500000;
    mstate.current_bpm = 120;
    mstate.elapsed_ms = 0;

    if (mstate.division > 0) {
        mstate.total_ms = (uint32_t)(((uint64_t)max_track_ticks * 500ULL) / (uint64_t)mstate.division);
    } else {
        mstate.total_ms = 60000;
    }

    if (mstate.song_title[0] == '\0') {
        strlcpy(mstate.song_title, "Standard MIDI Sequence", sizeof(mstate.song_title));
    }

    return loaded_tracks > 0;
}

static void apply_synth_config(void)
{
    const solar_os_synth_voice_performance_t perf = {
        .mono = false,
        .glide_ms = 0,
    };
    (void)solar_os_synth_voice_configure_performance(MIDIPLAYER_OWNER, &perf);

    solar_os_synth_voice_config_t config;
    memset(&config, 0, sizeof(config));
    config.waveform = mstate.waveform;
    config.attack_ms = 5;
    config.decay_ms = 80;
    config.sustain_percent = 70;
    config.release_ms = 120;
    config.filter.cutoff_hz = 14000;
    config.filter.resonance_percent = 0;
    (void)solar_os_synth_voice_configure(MIDIPLAYER_OWNER, &config);
}

static void midiplayer_load_builtin_demo(void)
{
    all_notes_off();
    mstate.is_builtin_demo = true;
    strlcpy(mstate.current_path, "builtin://demo", sizeof(mstate.current_path));
    strlcpy(mstate.song_title, "SolarOS Symphony", sizeof(mstate.song_title));
    if (mstate.file_buffer != NULL) {
        solar_os_memory_free(mstate.file_buffer);
        mstate.file_buffer = NULL;
    }
    parse_midi_header(builtin_demo_midi, sizeof(builtin_demo_midi));
}

static bool midiplayer_load_file(const char *path)
{
    if (path == NULL || path[0] == '\0') return false;
    all_notes_off();

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        SOLAR_OS_LOGW(TAG, "Cannot open MIDI file %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    const long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < 14 || fsize > 512 * 1024) {
        fclose(f);
        return false;
    }

    if (mstate.file_buffer != NULL) {
        solar_os_memory_free(mstate.file_buffer);
        mstate.file_buffer = NULL;
    }

    mstate.file_buffer = solar_os_memory_alloc((size_t)fsize, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "midi.buf");
    if (mstate.file_buffer == NULL) {
        fclose(f);
        return false;
    }

    const size_t read_bytes = fread(mstate.file_buffer, 1, (size_t)fsize, f);
    fclose(f);

    if (read_bytes != (size_t)fsize) {
        solar_os_memory_free(mstate.file_buffer);
        mstate.file_buffer = NULL;
        return false;
    }

    mstate.file_size = (size_t)fsize;
    mstate.is_builtin_demo = false;
    strlcpy(mstate.current_path, path, sizeof(mstate.current_path));
    mstate.song_title[0] = '\0';

    if (!parse_midi_header(mstate.file_buffer, mstate.file_size)) {
        solar_os_memory_free(mstate.file_buffer);
        mstate.file_buffer = NULL;
        return false;
    }

    return true;
}

static void start_playback(void)
{
    all_notes_off();
    mstate.current_tick = 0;
    mstate.elapsed_ms = 0;
    mstate.last_step_ms = (uint32_t)(esp_timer_get_time() / 1000U);
    for (size_t t = 0; t < mstate.track_count; t++) {
        reset_track(&mstate.tracks[t]);
    }
    mstate.playing = true;
    mstate.paused = false;
    apply_synth_config();
}

static void pause_playback(void)
{
    if (mstate.playing) {
        mstate.paused = !mstate.paused;
        if (mstate.paused) {
            all_notes_off();
        } else {
            mstate.last_step_ms = (uint32_t)(esp_timer_get_time() / 1000U);
        }
    } else {
        start_playback();
    }
}

static void stop_playback(void)
{
    mstate.playing = false;
    mstate.paused = false;
    all_notes_off();
    mstate.current_tick = 0;
    mstate.elapsed_ms = 0;
    for (size_t t = 0; t < mstate.track_count; t++) {
        reset_track(&mstate.tracks[t]);
    }
}

static void cycle_waveform(void)
{
    switch (mstate.waveform) {
    case SOLAR_OS_SYNTH_WAVE_SQUARE:
        mstate.waveform = SOLAR_OS_SYNTH_WAVE_TRIANGLE;
        break;
    case SOLAR_OS_SYNTH_WAVE_TRIANGLE:
        mstate.waveform = SOLAR_OS_SYNTH_WAVE_SINE;
        break;
    case SOLAR_OS_SYNTH_WAVE_SINE:
        mstate.waveform = SOLAR_OS_SYNTH_WAVE_SAW;
        break;
    case SOLAR_OS_SYNTH_WAVE_SAW:
    default:
        mstate.waveform = SOLAR_OS_SYNTH_WAVE_SQUARE;
        break;
    }
    apply_synth_config();
}

static void sequencer_step(uint32_t now_ms)
{
    if (!mstate.playing || mstate.paused) {
        mstate.last_step_ms = now_ms;
        return;
    }

    if (mstate.last_step_ms == 0) {
        mstate.last_step_ms = now_ms;
        return;
    }

    uint32_t elapsed_ms = now_ms - mstate.last_step_ms;
    if (elapsed_ms == 0) return;
    if (elapsed_ms > 100) elapsed_ms = 100; /* Clamp */
    mstate.last_step_ms = now_ms;

    mstate.elapsed_ms += elapsed_ms;

    const uint32_t us_per_tick = (mstate.division > 0 && mstate.tempo_us_per_qn > 0) ?
                                 (mstate.tempo_us_per_qn / mstate.division) : 5000;
    const uint32_t ticks_to_advance = (elapsed_ms * 1000U) / (us_per_tick > 0 ? us_per_tick : 1);
    mstate.current_tick += (ticks_to_advance > 0 ? ticks_to_advance : 1);

    bool any_track_active = false;

    for (size_t t = 0; t < mstate.track_count; t++) {
        midi_track_t *tr = &mstate.tracks[t];
        if (tr->end_of_track || tr->ptr >= tr->end) continue;

        any_track_active = true;

        while (!tr->end_of_track && tr->ptr < tr->end && tr->next_event_tick <= mstate.current_tick) {
            uint8_t status = *tr->ptr;
            if (status >= 0x80) {
                tr->ptr++;
                tr->running_status = status;
            } else {
                status = tr->running_status;
            }

            if (status == 0xFF) {
                if (tr->ptr >= tr->end) { tr->end_of_track = true; break; }
                const uint8_t meta_type = *tr->ptr++;
                const uint32_t meta_len = read_vlq(&tr->ptr, tr->end);

                if (meta_type == 0x51 && meta_len >= 3 && tr->ptr + 3 <= tr->end) {
                    mstate.tempo_us_per_qn = ((uint32_t)tr->ptr[0] << 16) | ((uint32_t)tr->ptr[1] << 8) | (uint32_t)tr->ptr[2];
                    if (mstate.tempo_us_per_qn > 0) {
                        mstate.current_bpm = 60000000U / mstate.tempo_us_per_qn;
                    }
                } else if (meta_type == 0x2F) {
                    tr->end_of_track = true;
                }
                tr->ptr += meta_len;
            } else if (status == 0xF0 || status == 0xF7) {
                const uint32_t sysex_len = read_vlq(&tr->ptr, tr->end);
                tr->ptr += sysex_len;
            } else {
                const uint8_t channel = status & 0x0F;
                const uint8_t cmd = status & 0xF0;

                if (cmd == 0x90) {
                    if (tr->ptr + 2 <= tr->end) {
                        const uint8_t note = *tr->ptr++;
                        const uint8_t vel = *tr->ptr++;
                        note_on(channel, note, vel);
                    }
                } else if (cmd == 0x80) {
                    if (tr->ptr + 2 <= tr->end) {
                        const uint8_t note = *tr->ptr++;
                        (void)*tr->ptr++;
                        note_off(channel, note);
                    }
                } else if (cmd == 0xC0 || cmd == 0xD0) {
                    if (tr->ptr + 1 <= tr->end) tr->ptr++;
                } else {
                    if (tr->ptr + 2 <= tr->end) tr->ptr += 2;
                }
            }

            if (!tr->end_of_track && tr->ptr < tr->end) {
                const uint32_t delta = read_vlq(&tr->ptr, tr->end);
                tr->next_event_tick += delta;
            } else {
                tr->end_of_track = true;
            }
        }
    }

    for (size_t i = 0; i < 16; i++) {
        if (mstate.channel_activity[i] > 8) {
            mstate.channel_activity[i] -= 8;
        } else {
            mstate.channel_activity[i] = 0;
        }
    }

    if (!any_track_active || (mstate.total_ticks > 0 && mstate.current_tick >= mstate.total_ticks + (mstate.division * 2))) {
        all_notes_off();
        if (mstate.loop) {
            start_playback();
        } else {
            mstate.playing = false;
            mstate.paused = false;
        }
    }
}

static void scan_playlist(void)
{
    mstate.playlist_count = 0;

    playlist_item_t *demo = &mstate.playlist[mstate.playlist_count++];
    demo->is_builtin = true;
    strlcpy(demo->path, "builtin://demo", sizeof(demo->path));
    strlcpy(demo->name, "[Built-in] SolarOS Symphony", sizeof(demo->name));

    static const char *const exts[] = {".mid", ".midi", ".kar"};

    if (solar_os_storage_sd_is_mounted()) {
        static const char *const sd_dirs[] = {"/sd/Music", "/sd/midi", "/sd/music", "/sd"};
        for (size_t d = 0; d < sizeof(sd_dirs) / sizeof(sd_dirs[0]); d++) {
            DIR *dir = opendir(sd_dirs[d]);
            if (dir == NULL) continue;
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                const char *dot = strrchr(entry->d_name, '.');
                if (dot == NULL) continue;
                for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
                    if (strcasecmp(dot, exts[e]) == 0) {
                        if (mstate.playlist_count < MIDIPLAYER_MAX_PLAYLIST) {
                            playlist_item_t *item = &mstate.playlist[mstate.playlist_count++];
                            item->is_builtin = false;
                            snprintf(item->path, sizeof(item->path), "%s/%s", sd_dirs[d], entry->d_name);
                            strlcpy(item->name, entry->d_name, sizeof(item->name));
                        }
                        break;
                    }
                }
            }
            closedir(dir);
        }
    }

    if (solar_os_storage_flash_is_mounted()) {
        DIR *dir = opendir("/flash/midi");
        if (dir != NULL) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                const char *dot = strrchr(entry->d_name, '.');
                if (dot == NULL) continue;
                for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
                    if (strcasecmp(dot, exts[e]) == 0) {
                        if (mstate.playlist_count < MIDIPLAYER_MAX_PLAYLIST) {
                            playlist_item_t *item = &mstate.playlist[mstate.playlist_count++];
                            item->is_builtin = false;
                            snprintf(item->path, sizeof(item->path), "/flash/midi/%s", entry->d_name);
                            strlcpy(item->name, entry->d_name, sizeof(item->name));
                        }
                        break;
                    }
                }
            }
            closedir(dir);
        }
    }
}

static void draw_piano_keyboard(solar_os_gfx_t *gfx, int kx, int ky)
{
    const int kw = 26;
    const int kh = 48;
    const int bkw = 16;
    const int bkh = 30;

    static const uint8_t white_notes[] = {
        48, 50, 52, 53, 55, 57, 59,
        60, 62, 64, 65, 67, 69, 71
    };

    static const uint8_t black_notes[] = {
        49, 51, 0, 54, 56, 58, 0,
        61, 63, 0, 66, 68, 70, 0
    };

    for (int i = 0; i < 14; i++) {
        const int wx = kx + i * kw;
        const uint8_t note = white_notes[i];

        bool is_active = false;
        for (size_t n = 0; n < MIDIPLAYER_MAX_ACTIVE_NOTES; n++) {
            if (mstate.active_notes[n].active && mstate.active_notes[n].note == note) {
                is_active = true;
                break;
            }
        }

        if (is_active) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, wx, ky, kw, kh);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_fill_rect(gfx, wx, ky, kw, kh);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_rect(gfx, wx, ky, kw, kh);
        }
    }

    for (int i = 0; i < 13; i++) {
        const uint8_t bnote = black_notes[i];
        if (bnote == 0) continue;

        const int bx = kx + (i + 1) * kw - (bkw / 2);

        bool is_active = false;
        for (size_t n = 0; n < MIDIPLAYER_MAX_ACTIVE_NOTES; n++) {
            if (mstate.active_notes[n].active && mstate.active_notes[n].note == bnote) {
                is_active = true;
                break;
            }
        }

        if (is_active) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
            solar_os_gfx_fill_rect(gfx, bx, ky, bkw, bkh);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_rect(gfx, bx, ky, bkw, bkh);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, bx, ky, bkw, bkh);
        }
    }
}

static void draw_player_view(solar_os_gfx_t *gfx)
{
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, 10, 36, 380, 72);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    char title_buf[48];
    snprintf(title_buf, sizeof(title_buf), "Song: %s", mstate.song_title);
    solar_os_gfx_text(gfx, 18, 52, title_buf);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    char fmt_buf[64];
    snprintf(fmt_buf, sizeof(fmt_buf), "Type %u | %u Tracks | %u TPQN | %u BPM",
             (unsigned)mstate.format, (unsigned)mstate.track_count, (unsigned)mstate.division, (unsigned)mstate.current_bpm);
    solar_os_gfx_text(gfx, 18, 68, fmt_buf);

    const uint32_t el_s = mstate.elapsed_ms / 1000U;
    const uint32_t tot_s = mstate.total_ms / 1000U;
    const int pct = (mstate.total_ticks > 0) ? (int)((mstate.current_tick * 100) / mstate.total_ticks) : 0;

    char time_str[64];
    snprintf(time_str, sizeof(time_str), "%02u:%02u / %02u:%02u (%d%%)  [%s]",
             (unsigned)(el_s / 60), (unsigned)(el_s % 60),
             (unsigned)(tot_s / 60), (unsigned)(tot_s % 60),
             pct > 100 ? 100 : pct,
             mstate.playing ? (mstate.paused ? "PAUSED" : "PLAYING") : "STOPPED");
    solar_os_gfx_text(gfx, 18, 86, time_str);

    solar_os_gfx_rect(gfx, 18, 92, 364, 10);
    const int fill_w = (360 * (pct > 100 ? 100 : pct)) / 100;
    if (fill_w > 0) {
        solar_os_gfx_fill_rect(gfx, 20, 94, fill_w, 6);
    }

    solar_os_gfx_rect(gfx, 10, 114, 380, 36);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_text(gfx, 16, 126, "CHANNELS (1-16):");

    for (int c = 0; c < 16; c++) {
        const int mx = 120 + c * 16;
        const int my = 120;
        const int mw = 12;
        const int mh = 24;

        solar_os_gfx_rect(gfx, mx, my, mw, mh);
        const int vh = (mstate.channel_activity[c] * (mh - 2)) / 127;
        if (vh > 0) {
            solar_os_gfx_fill_rect(gfx, mx + 1, my + mh - 1 - vh, mw - 2, vh);
        }
    }

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 18, 166, "KEYBOARD VISUALIZER (C3 - B4):");
    draw_piano_keyboard(gfx, 18, 172);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    const char *wave_name = solar_os_synth_waveform_name(mstate.waveform);
    char synth_info[96];
    snprintf(synth_info, sizeof(synth_info), "Wave: [W] %s  |  Vol: %u%%  |  Loop: %s",
             wave_name ? wave_name : "Square", (unsigned)mstate.volume, mstate.loop ? "ON" : "OFF");
    solar_os_gfx_text(gfx, 18, 236, synth_info);
}

static void draw_picker_view(solar_os_gfx_t *gfx)
{
    const int table_y = 38;
    const int row_h = 24;
    const size_t visible_rows = 8;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 10, table_y, 380, 18);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, 16, table_y + 13, "SELECT MIDI FILE (*.mid, *.midi)");

    int cur_y = table_y + 20;

    if (mstate.playlist_count == 0) {
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 110, cur_y + 30, "No MIDI files found");
        return;
    }

    const size_t start_idx = mstate.picker_scroll_offset;
    const size_t end_idx = (start_idx + visible_rows < mstate.playlist_count) ?
                           (start_idx + visible_rows) : mstate.playlist_count;

    for (size_t i = start_idx; i < end_idx; i++) {
        const playlist_item_t *item = &mstate.playlist[i];
        const bool is_sel = (i == mstate.selected_file_idx);

        if (is_sel) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_fill_rect(gfx, 10, cur_y, 380, row_h - 2);
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        } else {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_line(gfx, 10, cur_y + row_h - 2, 390, cur_y + row_h - 2);
        }

        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, 16, cur_y + 16, item->name);

        cur_y += row_h;
    }
}

static void draw_help_modal(solar_os_gfx_t *gfx)
{
    const int hx = 16;
    const int hy = 20;
    const int hw = 368;
    const int hh = 256;

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, hx, hy, hw, hh);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, hx, hy, hw, hh);
    solar_os_gfx_rect(gfx, hx + 1, hy + 1, hw - 2, hh - 2);

    solar_os_gfx_line(gfx, hx, hy + 24, hx + hw, hy + 24);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, hx + 12, hy + 18, "MIDI Player Help & Shortcuts");

    int text_y = hy + 44;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, hx + 14, text_y, "Playback Controls:");

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    text_y += 18;
    solar_os_gfx_text(gfx, hx + 14, text_y, "- [Space]: Play / Pause toggle");
    text_y += 16;
    solar_os_gfx_text(gfx, hx + 14, text_y, "- [S]: Stop playback and reset to start");
    text_y += 16;
    solar_os_gfx_text(gfx, hx + 14, text_y, "- [Left] / [Right]: Seek -5s / +5s");
    text_y += 16;
    solar_os_gfx_text(gfx, hx + 14, text_y, "- [Up] / [Down]: Volume up / down");
    text_y += 16;
    solar_os_gfx_text(gfx, hx + 14, text_y, "- [W]: Cycle synth waveform (Square, Sine, Saw, Tri)");
    text_y += 16;
    solar_os_gfx_text(gfx, hx + 14, text_y, "- [L]: Toggle single song loop on/off");
    text_y += 16;
    solar_os_gfx_text(gfx, hx + 14, text_y, "- [O]: Open file picker / playlist");
    text_y += 16;
    solar_os_gfx_text(gfx, hx + 14, text_y, "- [ESC]: Minimize (keeps playing in background) / Exit");

    solar_os_gfx_line(gfx, hx, hy + hh - 24, hx + hw, hy + hh - 24);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_text(gfx, hx + 40, hy + hh - 8, "Press [ESC], [Enter], [?] or click to close");
}

static void midiplayer_render(solar_os_context_t *ctx)
{
    if (ctx == NULL) return;
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) return;

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    const solar_os_appbar_header_t header = {
        .title = "midiplayer",
        .show_back = true,
    };
    solar_os_appbar_draw_header(gfx, &header);

    if (mstate.view == MIDIPLAYER_VIEW_PICKER) {
        draw_picker_view(gfx);
    } else {
        draw_player_view(gfx);
    }

    solar_os_appbar_shortcut_t shortcuts[SOLAR_OS_APPBAR_SHORTCUT_MAX];
    size_t sc_count = 0;

    if (mstate.view == MIDIPLAYER_VIEW_PICKER) {
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = '\r', .label = "Play" };
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = 'C', .label = "Cancel" };
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = '?', .label = "Help" };
    } else {
        if (mstate.playing && !mstate.paused) {
            shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = ' ', .label = "Pause" };
        } else {
            shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = ' ', .label = "Play" };
        }
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = 'S', .label = "Stop" };
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = 'O', .label = "Open" };
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = 'W', .label = "Wave" };
        shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = '?', .label = "Help" };
    }

    const solar_os_appbar_shortcuts_t footer = {
        .items = shortcuts,
        .count = sc_count,
    };
    solar_os_appbar_draw_footer(gfx, &footer);

    if (mstate.show_help) {
        draw_help_modal(gfx);
    }

    solar_os_gfx_present(gfx);
}

static esp_err_t midiplayer_start(solar_os_context_t *ctx)
{
    if (ctx == NULL) return ESP_ERR_INVALID_ARG;
    mstate.ctx = ctx;
    mstate.view = MIDIPLAYER_VIEW_PLAYER;
    mstate.volume = 85;
    mstate.waveform = SOLAR_OS_SYNTH_WAVE_SQUARE;
    mstate.loop = true;
    mstate.show_help = false;
    mstate.last_step_ms = 0;
    mstate.last_render_ms = 0;
    mstate.click_note = 0;
    mstate.click_release_at_ms = 0;

    solar_os_context_set_graphics_active(ctx, true);
    scan_playlist();

    const int argc = solar_os_context_argc(ctx);
    if (argc >= 2) {
        const char *arg_file = solar_os_context_argv(ctx, 1);
        if (arg_file != NULL && arg_file[0] != '\0' && strcmp(arg_file, "builtin://demo") != 0) {
            if (!midiplayer_load_file(arg_file)) {
                midiplayer_load_builtin_demo();
            }
        } else {
            midiplayer_load_builtin_demo();
        }
    } else {
        midiplayer_load_builtin_demo();
    }

    start_playback();
    midiplayer_render(ctx);
    return ESP_OK;
}

static void midiplayer_stop(solar_os_context_t *ctx)
{
    mstate.playing = false;
    all_notes_off();
    solar_os_synth_voice_stop(MIDIPLAYER_OWNER);

    if (mstate.file_buffer != NULL) {
        solar_os_memory_free(mstate.file_buffer);
        mstate.file_buffer = NULL;
    }
    if (ctx != NULL) {
        solar_os_context_set_graphics_active(ctx, false);
    }
}

static void midiplayer_suspend(solar_os_context_t *ctx)
{
    if (ctx != NULL) {
        solar_os_context_set_graphics_active(ctx, false);
    }
}

static void midiplayer_resume(solar_os_context_t *ctx)
{
    if (ctx != NULL) {
        solar_os_context_set_graphics_active(ctx, true);
        mstate.last_step_ms = (uint32_t)(esp_timer_get_time() / 1000U);
        midiplayer_render(ctx);
    }
}

static bool handle_input_action(solar_os_context_t *ctx, uint8_t key)
{
    if (mstate.show_help) {
        if (key == SOLAR_OS_KEY_ESCAPE || key == '\r' || key == '\n' ||
            key == '?' || key == 'h' || key == 'H' || key == 'q' || key == 'Q') {
            mstate.show_help = false;
            midiplayer_render(ctx);
            return true;
        }
        return true;
    }

    if (mstate.view == MIDIPLAYER_VIEW_PICKER) {
        if (key == SOLAR_OS_KEY_UP || key == 'k' || key == 'K') {
            if (mstate.selected_file_idx > 0) mstate.selected_file_idx--;
            if (mstate.selected_file_idx < mstate.picker_scroll_offset) {
                mstate.picker_scroll_offset = mstate.selected_file_idx;
            }
            midiplayer_render(ctx);
            return true;
        }
        if (key == SOLAR_OS_KEY_DOWN || key == 'j' || key == 'J') {
            if (mstate.selected_file_idx + 1 < mstate.playlist_count) mstate.selected_file_idx++;
            if (mstate.selected_file_idx >= mstate.picker_scroll_offset + 8) {
                mstate.picker_scroll_offset = mstate.selected_file_idx - 7;
            }
            midiplayer_render(ctx);
            return true;
        }
        if (key == '\r' || key == '\n') {
            if (mstate.selected_file_idx < mstate.playlist_count) {
                if (mstate.playlist[mstate.selected_file_idx].is_builtin) {
                    midiplayer_load_builtin_demo();
                } else {
                    midiplayer_load_file(mstate.playlist[mstate.selected_file_idx].path);
                }
                start_playback();
                mstate.view = MIDIPLAYER_VIEW_PLAYER;
                midiplayer_render(ctx);
            }
            return true;
        }
        if (key == 'c' || key == 'C' || key == SOLAR_OS_KEY_ESCAPE) {
            mstate.view = MIDIPLAYER_VIEW_PLAYER;
            midiplayer_render(ctx);
            return true;
        }
        return false;
    }

    if (key == ' ') {
        pause_playback();
        midiplayer_render(ctx);
        return true;
    }
    if (key == 's' || key == 'S') {
        stop_playback();
        midiplayer_render(ctx);
        return true;
    }
    if (key == 'o' || key == 'O') {
        scan_playlist();
        mstate.view = MIDIPLAYER_VIEW_PICKER;
        midiplayer_render(ctx);
        return true;
    }
    if (key == 'w' || key == 'W') {
        cycle_waveform();
        midiplayer_render(ctx);
        return true;
    }
    if (key == 'l' || key == 'L') {
        mstate.loop = !mstate.loop;
        midiplayer_render(ctx);
        return true;
    }
    if (key == SOLAR_OS_KEY_UP || key == '+' || key == '=') {
        if (mstate.volume <= 95) mstate.volume += 5;
        else mstate.volume = 100;
        apply_synth_config();
        midiplayer_render(ctx);
        return true;
    }
    if (key == SOLAR_OS_KEY_DOWN || key == '-' || key == '_') {
        if (mstate.volume >= 5) mstate.volume -= 5;
        else mstate.volume = 0;
        apply_synth_config();
        midiplayer_render(ctx);
        return true;
    }
    if (key == SOLAR_OS_KEY_LEFT) {
        const uint32_t ticks_5s = mstate.division > 0 ? (mstate.division * 10) : 1000;
        if (mstate.current_tick > ticks_5s) {
            mstate.current_tick -= ticks_5s;
        } else {
            mstate.current_tick = 0;
        }
        all_notes_off();
        for (size_t t = 0; t < mstate.track_count; t++) {
            reset_track(&mstate.tracks[t]);
        }
        midiplayer_render(ctx);
        return true;
    }
    if (key == SOLAR_OS_KEY_RIGHT) {
        const uint32_t ticks_5s = mstate.division > 0 ? (mstate.division * 10) : 1000;
        if (mstate.current_tick + ticks_5s < mstate.total_ticks) {
            mstate.current_tick += ticks_5s;
        }
        all_notes_off();
        for (size_t t = 0; t < mstate.track_count; t++) {
            reset_track(&mstate.tracks[t]);
        }
        midiplayer_render(ctx);
        return true;
    }
    if (key == '?' || key == 'h' || key == 'H') {
        mstate.show_help = !mstate.show_help;
        midiplayer_render(ctx);
        return true;
    }
    if (key == SOLAR_OS_KEY_ESCAPE || key == 'q' || key == 'Q') {
        solar_os_context_request_exit(ctx);
        return true;
    }

    return false;
}

static bool midiplayer_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (ctx == NULL || event == NULL) return false;

    if (event->type == SOLAR_OS_EVENT_RESUME) {
        midiplayer_resume(ctx);
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000U);

        /* 1. Step sequencer */
        sequencer_step(now);

        /* 2. Auto-release tapped piano key */
        if (mstate.click_release_at_ms != 0 && now >= mstate.click_release_at_ms) {
            note_off(0, mstate.click_note);
            mstate.click_release_at_ms = 0;
            mstate.click_note = 0;
            midiplayer_render(ctx);
        }

        /* 3. Render visualizer at 12 FPS */
        if (mstate.playing && !mstate.paused && (now - mstate.last_render_ms >= MIDIPLAYER_REFRESH_MS)) {
            mstate.last_render_ms = now;
            midiplayer_render(ctx);
        }
        return true;
    }

    if (event->type == SOLAR_OS_EVENT_CLICK) {
        solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
        if (gfx == NULL) return false;

        if (mstate.show_help) {
            mstate.show_help = false;
            midiplayer_render(ctx);
            return true;
        }

        const int px = event->data.click.x;
        const int py = event->data.click.y;

        /* 1. Header click */
        const solar_os_appbar_header_t header = { .title = "midiplayer", .show_back = true };
        solar_os_appbar_hit_t hit;
        if (solar_os_appbar_hit_test_header(gfx, &header, px, py, &hit)) {
            if (hit.kind == SOLAR_OS_APPBAR_HIT_BACK) {
                if (mstate.view == MIDIPLAYER_VIEW_PICKER) {
                    mstate.view = MIDIPLAYER_VIEW_PLAYER;
                    midiplayer_render(ctx);
                } else {
                    solar_os_context_request_exit(ctx);
                }
                return true;
            }
        }

        /* 2. File Picker click */
        if (mstate.view == MIDIPLAYER_VIEW_PICKER && py >= 58 && py <= 245) {
            const size_t idx = (size_t)((py - 58) / 24) + mstate.picker_scroll_offset;
            if (idx < mstate.playlist_count) {
                mstate.selected_file_idx = idx;
                if (mstate.playlist[idx].is_builtin) {
                    midiplayer_load_builtin_demo();
                } else {
                    midiplayer_load_file(mstate.playlist[idx].path);
                }
                start_playback();
                mstate.view = MIDIPLAYER_VIEW_PLAYER;
                midiplayer_render(ctx);
                return true;
            }
        }

        /* 3. Piano Key Touch */
        if (mstate.view == MIDIPLAYER_VIEW_PLAYER && py >= 172 && py <= 220) {
            const int kw = 26;
            const int key_idx = (px - 18) / kw;
            if (key_idx >= 0 && key_idx < 14) {
                static const uint8_t notes[] = {48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71};
                if (mstate.click_note != 0) {
                    note_off(0, mstate.click_note);
                }
                mstate.click_note = notes[key_idx];
                mstate.click_release_at_ms = (uint32_t)(esp_timer_get_time() / 1000U) + 250U;
                note_on(0, mstate.click_note, 100);
                midiplayer_render(ctx);
                return true;
            }
        }

        /* 4. Footer Buttons Hit Test */
        solar_os_appbar_shortcut_t shortcuts[SOLAR_OS_APPBAR_SHORTCUT_MAX];
        size_t sc_count = 0;
        if (mstate.view == MIDIPLAYER_VIEW_PICKER) {
            shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = '\r', .label = "Play" };
            shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = 'C', .label = "Cancel" };
            shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = '?', .label = "Help" };
        } else {
            if (mstate.playing && !mstate.paused) {
                shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = ' ', .label = "Pause" };
            } else {
                shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = ' ', .label = "Play" };
            }
            shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = 'S', .label = "Stop" };
            shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = 'O', .label = "Open" };
            shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = 'W', .label = "Wave" };
            shortcuts[sc_count++] = (solar_os_appbar_shortcut_t){ .key = '?', .label = "Help" };
        }
        const solar_os_appbar_shortcuts_t footer = { .items = shortcuts, .count = sc_count };
        solar_os_appbar_hit_t fhit;
        if (solar_os_appbar_hit_test_footer(gfx, &footer, px, py, &fhit)) {
            if (fhit.kind == SOLAR_OS_APPBAR_HIT_FOOTER_ITEM && fhit.index < sc_count) {
                handle_input_action(ctx, (uint8_t)shortcuts[fhit.index].key);
                return true;
            }
        }
    }

    if (event->type == SOLAR_OS_EVENT_KEY) {
        if (event->data.key.action == SOLAR_OS_INPUT_KEY_PRESS) {
            return handle_input_action(ctx, (uint8_t)event->data.key.key);
        }
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_CHAR) {
        return handle_input_action(ctx, (uint8_t)event->data.ch);
    }

    return false;
}

const solar_os_app_t solar_os_midiplayer_app = {
    .name = "midiplayer",
    .summary = "Standard MIDI File player and synthesizer",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = midiplayer_start,
    .stop = midiplayer_stop,
    .suspend = midiplayer_suspend,
    .resume = midiplayer_resume,
    .event = midiplayer_event,
    .state_slot = &midiplayer_state_ptr,
    .state_size = sizeof(midiplayer_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .tick_interval_ms = 20U, /* 50 Hz tick for super smooth sequencing */
    .worker_stack_bytes = 0,
};
